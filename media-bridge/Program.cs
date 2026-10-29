using System.Text.Json;
using Windows.Graphics.Imaging;
using Windows.Media.Control;
using Windows.Storage.Streams;

var builder = WebApplication.CreateBuilder(args);
builder.WebHost.UseUrls("http://0.0.0.0:8080");
builder.Services.ConfigureHttpJsonOptions(o =>
    o.SerializerOptions.PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower);
builder.Services.AddSingleton<MediaState>();
builder.Services.AddHostedService<MediaPoller>();

var app = builder.Build();

app.MapGet("/", () => "MediaBridge is running. Try /now-playing and /artwork");

app.MapGet("/now-playing", (HttpContext ctx, MediaState st) =>
{
    var s = st.Current;
    int v = st.ArtVersion;
    string? artUrl = v > 0 ? $"{ctx.Request.Scheme}://{ctx.Request.Host}/artwork?v={v}" : null;
    ctx.Response.Headers.CacheControl = "no-store";
    return Results.Json(new NowPlaying(s.Title, s.Artist, s.Album, s.State,
        s.PositionMs, s.DurationMs, artUrl, s.UpdatedAt, s.App));
});

app.MapGet("/artwork", (HttpContext ctx, MediaState st) =>
{
    var a = st.Art;
    ctx.Response.Headers.CacheControl = "no-store";
    return a == null ? Results.NotFound() : Results.File(a, "image/jpeg");
});

Console.WriteLine("MediaBridge listening on port 8080");
app.Run();

// ---------- types ----------

record NowPlaying(string Title, string Artist, string Album, string State,
                  long PositionMs, long DurationMs, string? ArtUrl, long UpdatedAt, string App);

record Snapshot(string App, string Title, string Artist, string Album, string State,
                long PositionMs, long DurationMs, long UpdatedAt)
{
    public static Snapshot Empty =>
        new("", "", "", "", "stopped", 0, 0, DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
}

class MediaState
{
    public volatile Snapshot Current = Snapshot.Empty;
    byte[]? art;
    int artCounter;

    public byte[]? Art => art;
    public bool HasArt => art != null;
    public int ArtVersion => art == null ? 0 : artCounter;

    public void SetArt(byte[] bytes) { artCounter++; art = bytes; }
    public void ClearArt() { art = null; }
}

class MediaPoller(MediaState state) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken ct)
    {
        GlobalSystemMediaTransportControlsSessionManager? manager = null;
        string lastTrackKey = "";
        string lastLogKey = "";
        int artTries = 0;

        while (!ct.IsCancellationRequested)
        {
            try
            {
                manager ??= await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
                var session = manager.GetCurrentSession();
                long now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();

                if (session == null)
                {
                    state.Current = Snapshot.Empty;
                    state.ClearArt();
                    lastTrackKey = "";
                    if (lastLogKey != "NONE") { Console.WriteLine("No active media session"); lastLogKey = "NONE"; }
                }
                else
                {
                    var props = await session.TryGetMediaPropertiesAsync();
                    var playback = session.GetPlaybackInfo();
                    var timeline = session.GetTimelineProperties();

                    string app = session.SourceAppUserModelId;
                    var status = playback.PlaybackStatus;
                    string stateStr = status switch
                    {
                        GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing => "playing",
                        GlobalSystemMediaTransportControlsSessionPlaybackStatus.Paused => "paused",
                        _ => "stopped"
                    };

                    TimeSpan duration = timeline.EndTime - timeline.StartTime;
                    TimeSpan position = timeline.Position - timeline.StartTime;
                    if (status == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing)
                        position += DateTimeOffset.Now - timeline.LastUpdatedTime;
                    if (position < TimeSpan.Zero) position = TimeSpan.Zero;
                    if (duration <= TimeSpan.Zero) duration = TimeSpan.Zero;
                    else if (position > duration) position = duration;

                    // New track: forget the old artwork
                    string trackKey = $"{app}|{props.Title}|{props.Artist}|{props.AlbumTitle}";
                    if (trackKey != lastTrackKey)
                    {
                        lastTrackKey = trackKey;
                        artTries = 0;
                        state.ClearArt();
                    }

                    // Thumbnail sometimes arrives a moment after the text, so retry a few times
                    if (!state.HasArt && props.Thumbnail != null && artTries < 5)
                    {
                        artTries++;
                        var jpg = await EncodeArtAsync(props.Thumbnail);
                        if (jpg != null)
                        {
                            state.SetArt(jpg);
                            Console.WriteLine($"   artwork encoded: {jpg.Length} bytes");
                        }
                    }

                    state.Current = new Snapshot(app, props.Title ?? "", props.Artist ?? "",
                        props.AlbumTitle ?? "", stateStr,
                        (long)position.TotalMilliseconds, (long)duration.TotalMilliseconds, now);

                    string logKey = $"{trackKey}|{stateStr}";
                    if (logKey != lastLogKey)
                    {
                        lastLogKey = logKey;
                        Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] {stateStr}: {props.Artist} - {props.Title} ({app})");
                    }
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] ERROR: {ex.GetType().Name}: {ex.Message}");
                manager = null;   // re-request the manager on the next loop
            }

            try { await Task.Delay(1000, ct); } catch (OperationCanceledException) { }
        }
    }

    // Decode whatever Windows gives us, centre-crop to a square, scale to 160x160, write baseline JPEG
    static async Task<byte[]?> EncodeArtAsync(IRandomAccessStreamReference thumbnail)
    {
        try
        {
            const uint size = 160;
            using var src = await thumbnail.OpenReadAsync();
            var decoder = await BitmapDecoder.CreateAsync(src);
            uint w = decoder.PixelWidth, h = decoder.PixelHeight;
            if (w == 0 || h == 0) return null;

            double scale = (double)size / Math.Min(w, h);
            uint sw = Math.Max(size, (uint)Math.Round(w * scale));
            uint sh = Math.Max(size, (uint)Math.Round(h * scale));

            var transform = new BitmapTransform
            {
                ScaledWidth = sw,
                ScaledHeight = sh,
                InterpolationMode = BitmapInterpolationMode.Fant,
                Bounds = new BitmapBounds { X = (sw - size) / 2, Y = (sh - size) / 2, Width = size, Height = size }
            };

            var pixels = await decoder.GetPixelDataAsync(
                BitmapPixelFormat.Bgra8, BitmapAlphaMode.Ignore, transform,
                ExifOrientationMode.IgnoreExifOrientation, ColorManagementMode.DoNotColorManage);

            using var mem = new InMemoryRandomAccessStream();
            var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.JpegEncoderId, mem);
            encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Ignore,
                size, size, 96, 96, pixels.DetachPixelData());
            await encoder.FlushAsync();

            var bytes = new byte[mem.Size];
            mem.Seek(0);
            using var reader = new DataReader(mem);
            await reader.LoadAsync((uint)mem.Size);
            reader.ReadBytes(bytes);
            return bytes;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"   artwork encode failed: {ex.GetType().Name}: {ex.Message}");
            return null;
        }
    }
}