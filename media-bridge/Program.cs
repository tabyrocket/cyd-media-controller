using System.Collections.Concurrent;
using System.Net.WebSockets;
using System.Text.Json;
using System.Threading.Channels;
using Windows.Graphics.Imaging;
using Windows.Media.Control;
using Windows.Storage.Streams;

var builder = WebApplication.CreateBuilder(args);
builder.WebHost.UseUrls("http://0.0.0.0:8080");
builder.Services.ConfigureHttpJsonOptions(o =>
    o.SerializerOptions.PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower);
builder.Services.AddSingleton<MediaState>();
builder.Services.AddSingleton<Hub>();
builder.Services.AddSingleton<Controller>();
builder.Services.AddHostedService<MediaPoller>();

var app = builder.Build();
app.UseWebSockets(new WebSocketOptions { KeepAliveInterval = TimeSpan.FromSeconds(15) });

app.MapGet("/", () => "MediaBridge is running. Try /now-playing, /artwork, /text-test?s=..., and ws://<ip>:8080/media");

app.MapGet("/now-playing", (HttpContext ctx, MediaState st) =>
{
    ctx.Response.Headers.CacheControl = "no-store";
    return Results.Json(Json.Build(st, $"{ctx.Request.Scheme}://{ctx.Request.Host}"));
});

app.MapGet("/artwork", (HttpContext ctx, MediaState st) =>
{
    var a = st.Art;
    ctx.Response.Headers.CacheControl = "no-store";
    return a == null ? Results.NotFound() : Results.File(a, "image/jpeg");
});

// Alpha-mask bitmap for one text line: /text?line=title|artist|album&v=N
app.MapGet("/text", (HttpContext ctx, MediaState st, string? line) =>
{
    ctx.Response.Headers.CacheControl = "no-store";
    var t = st.Text;
    byte[]? b = line switch { "title" => t.Title, "artist" => t.Artist, "album" => t.Album, _ => null };
    return b == null ? Results.NotFound() : Results.File(b, "application/octet-stream");
});

// Debug: see exactly what the ESP32 would receive for any string, as a PNG.
// e.g. http://<ip>:8080/text-test?s=夜に駆ける&line=title   (line=artist for the small size)
app.MapGet("/text-test", (string s, string? line) =>
{
    var m = TextRender.Render(s, line != "artist" && line != "album", force: true);
    return m == null ? Results.NotFound() : Results.File(TextRender.MaskToPng(m), "image/png");
});

app.Map("/media", async (HttpContext ctx, MediaState st, Hub hub, Controller ctrl) =>
{
    if (!ctx.WebSockets.IsWebSocketRequest) { ctx.Response.StatusCode = 400; return; }

    using var ws = await ctx.WebSockets.AcceptWebSocketAsync();
    var (id, ch) = hub.Register();
    var ct = ctx.RequestAborted;
    string baseUrl = $"{ctx.Request.Scheme}://{ctx.Request.Host}";
    Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] WS client connected: {ctx.Connection.RemoteIpAddress} ({hub.Count} total)");

    // Reads commands from the client, and watches for it closing or vanishing
    var recv = Task.Run(async () =>
    {
        var buf = new byte[512];
        try
        {
            while (ws.State == WebSocketState.Open)
            {
                var r = await ws.ReceiveAsync(buf, ct);
                if (r.MessageType == WebSocketMessageType.Close) break;
                if (r.MessageType == WebSocketMessageType.Text && r.EndOfMessage)
                {
                    string text = System.Text.Encoding.UTF8.GetString(buf, 0, r.Count);
                    _ = ctrl.HandleAsync(text);
                }
            }
        }
        catch { }
        ch.Writer.TryWrite(0);
    });

    try
    {
        while (ws.State == WebSocketState.Open && !recv.IsCompleted && !ct.IsCancellationRequested)
        {
            var json = JsonSerializer.SerializeToUtf8Bytes(Json.Build(st, baseUrl), Json.Options);
            await ws.SendAsync(json, WebSocketMessageType.Text, true, ct);

            // Wait for a change signal, or send a heartbeat after 5 s
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            cts.CancelAfter(TimeSpan.FromSeconds(5));
            try { await ch.Reader.WaitToReadAsync(cts.Token); }
            catch (OperationCanceledException) when (!ct.IsCancellationRequested) { }
            while (ch.Reader.TryRead(out _)) { }
        }
    }
    catch (OperationCanceledException) { }
    catch (WebSocketException) { }
    finally
    {
        hub.Unregister(id);
        Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] WS client left ({hub.Count} total)");
    }
});

Console.WriteLine("MediaBridge listening on port 8080");
app.Run();

// ---------- types ----------

static class Json
{
    public static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower
    };

    public static NowPlaying Build(MediaState st, string baseUrl)
    {
        var s = st.Current;
        int v = st.ArtVersion;
        string? artUrl = v > 0 ? $"{baseUrl}/artwork?v={v}" : null;

        var tx = st.Text;
        string? TextUrl(byte[]? b, string line) => b == null ? null : $"{baseUrl}/text?line={line}&v={tx.Version}";

        return new NowPlaying(s.Title, s.Artist, s.Album, s.State,
            s.PositionMs, s.DurationMs, artUrl, s.UpdatedAt, s.App,
            v > 0 ? st.ArtColors : null,
            TextUrl(tx.Title, "title"), TextUrl(tx.Artist, "artist"), TextUrl(tx.Album, "album"));
    }
}

record NowPlaying(string Title, string Artist, string Album, string State,
                  long PositionMs, long DurationMs, string? ArtUrl, long UpdatedAt, string App,
                  int[]? ArtColors,
                  string? TitleImg, string? ArtistImg, string? AlbumImg);

record Snapshot(string App, string Title, string Artist, string Album, string State,
                long PositionMs, long DurationMs, long UpdatedAt)
{
    public static Snapshot Empty =>
        new("", "", "", "", "stopped", 0, 0, DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
}

// Alpha masks for lines that contain non-ASCII text (null = the ESP32 draws it with its own font)
record TextSet(int Version, byte[]? Title, byte[]? Artist, byte[]? Album)
{
    public static TextSet Empty => new(0, null, null, null);
}

class MediaState
{
    public volatile Snapshot Current = Snapshot.Empty;
    public volatile TextSet Text = TextSet.Empty;
    byte[]? art;
    int[]? artColors;
    int artCounter;
    int textCounter;

    public byte[]? Art => art;
    public int[]? ArtColors => artColors;
    public bool HasArt => art != null;
    public int ArtVersion => art == null ? 0 : artCounter;

    public void SetArt(byte[] bytes, int[] colors) { artCounter++; artColors = colors; art = bytes; }
    public void ClearArt() { art = null; artColors = null; }
    public void SetText(byte[]? title, byte[]? artist, byte[]? album) =>
        Text = new TextSet(++textCounter, title, artist, album);
}

// Tracks connected WebSocket clients and wakes them when something changed
class Hub
{
    readonly ConcurrentDictionary<Guid, Channel<byte>> clients = new();
    public int Count => clients.Count;

    public (Guid, Channel<byte>) Register()
    {
        var ch = Channel.CreateBounded<byte>(new BoundedChannelOptions(1)
            { FullMode = BoundedChannelFullMode.DropOldest });
        var id = Guid.NewGuid();
        clients[id] = ch;
        return (id, ch);
    }

    public void Unregister(Guid id) => clients.TryRemove(id, out _);

    public void Notify()
    {
        foreach (var c in clients.Values) c.Writer.TryWrite(0);
    }
}

class MediaPoller(MediaState state, Hub hub) : BackgroundService
{
    protected override async Task ExecuteAsync(CancellationToken ct)
    {
        GlobalSystemMediaTransportControlsSessionManager? manager = null;
        string lastTrackKey = "";
        string lastLogKey = "";
        string lastNotifyKey = "";
        long lastNotifyPos = 0, lastNotifyTime = 0;
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
                    if (lastTrackKey != "") state.SetText(null, null, null);
                    lastTrackKey = "";
                    if (lastLogKey != "NONE") { Console.WriteLine("No active media session"); lastLogKey = "NONE"; }
                    if (lastNotifyKey != "NONE") { lastNotifyKey = "NONE"; hub.Notify(); }
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

                    string trackKey = $"{app}|{props.Title}|{props.Artist}|{props.AlbumTitle}";
                    if (trackKey != lastTrackKey)
                    {
                        lastTrackKey = trackKey;
                        artTries = 0;
                        state.ClearArt();
                        // Lines with non-ASCII text become alpha masks; plain ASCII lines stay null
                        state.SetText(
                            TextRender.Render(props.Title, title: true),
                            TextRender.Render(props.Artist, title: false),
                            TextRender.Render(props.AlbumTitle, title: false));
                    }

                    if (!state.HasArt && props.Thumbnail != null && artTries < 5)
                    {
                        artTries++;
                        var res = await EncodeArtAsync(props.Thumbnail);
                        if (res != null)
                        {
                            state.SetArt(res.Value.Jpg, res.Value.Colors);
                            Console.WriteLine($"   artwork encoded: {res.Value.Jpg.Length} bytes");
                        }
                    }

                    long posMs = (long)position.TotalMilliseconds;
                    state.Current = new Snapshot(app, props.Title ?? "", props.Artist ?? "",
                        props.AlbumTitle ?? "", stateStr,
                        posMs, (long)duration.TotalMilliseconds, now);

                    // Push changes to clients
                    string notifyKey = $"{trackKey}|{stateStr}|{state.ArtVersion}|{(long)duration.TotalMilliseconds}";
                    bool seek = stateStr == "playing" &&
                                Math.Abs(posMs - (lastNotifyPos + (now - lastNotifyTime))) > 1500;
                    if (notifyKey != lastNotifyKey || seek)
                    {
                        lastNotifyKey = notifyKey;
                        lastNotifyPos = posMs;
                        lastNotifyTime = now;
                        hub.Notify();
                    }

                    string logKey = $"{trackKey}|{stateStr}";
                    if (logKey != lastLogKey)
                    {
                        lastLogKey = logKey;
                        var tx = state.Text;
                        Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] {stateStr}: {props.Artist} - {props.Title} ({app})" +
                            $"  [text masks: title={(tx.Title?.Length ?? 0)}B artist={(tx.Artist?.Length ?? 0)}B album={(tx.Album?.Length ?? 0)}B]");
                    }
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] ERROR: {ex.GetType().Name}: {ex.Message}");
                manager = null;
            }

            try { await Task.Delay(500, ct); } catch (OperationCanceledException) { }
        }
    }

        // Saturation-weighted average colour of the top half and bottom half of a BGRA image
    static int[] HalfColors(byte[] px, int size)
    {
        var res = new int[6];
        for (int half = 0; half < 2; half++)
        {
            double sr = 0, sg = 0, sb = 0, sw = 0;
            int y0 = half * size / 2, y1 = (half + 1) * size / 2;
            for (int y = y0; y < y1; y++)
                for (int x = 0; x < size; x++)
                {
                    int i = (y * size + x) * 4;
                    int b = px[i], g = px[i + 1], r = px[i + 2];
                    int mx = Math.Max(r, Math.Max(g, b)), mn = Math.Min(r, Math.Min(g, b));
                    double w = 0.05 + (mx - mn) / 255.0;   // vivid pixels count more than grey ones
                    sr += r * w; sg += g * w; sb += b * w; sw += w;
                }
            res[half * 3]     = (int)(sr / sw);
            res[half * 3 + 1] = (int)(sg / sw);
            res[half * 3 + 2] = (int)(sb / sw);
        }
        return res;
    }

    static async Task<(byte[] Jpg, int[] Colors)?> EncodeArtAsync(IRandomAccessStreamReference thumbnail)
    {
        try
        {
            const uint size = 128;
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

            byte[] px = pixels.DetachPixelData();
            int[] colors = HalfColors(px, (int)size);

            using var mem = new InMemoryRandomAccessStream();
            var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.JpegEncoderId, mem);
            encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Ignore,
                size, size, 96, 96, px);
            await encoder.FlushAsync();

            var bytes = new byte[mem.Size];
            mem.Seek(0);
            using var reader = new DataReader(mem);
            await reader.LoadAsync((uint)mem.Size);
            reader.ReadBytes(bytes);
            return (bytes, colors);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"   artwork encode failed: {ex.GetType().Name}: {ex.Message}");
            return null;
        }
    }
}

// Runs button commands from the ESP32 against the current media session
class Controller
{
    GlobalSystemMediaTransportControlsSessionManager? manager;

    public async Task HandleAsync(string text)
    {
        try
        {
            using var doc = JsonDocument.Parse(text);
            string cmd = doc.RootElement.GetProperty("cmd").GetString() ?? "";

            manager ??= await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
            var s = manager.GetCurrentSession();
            if (s == null)
            {
                Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] cmd '{cmd}' ignored: no active session");
                return;
            }

            bool ok;
            switch (cmd)
            {
                case "next":   ok = await s.TrySkipNextAsync(); break;
                case "prev":   ok = await s.TrySkipPreviousAsync(); break;
                case "toggle": ok = await s.TryTogglePlayPauseAsync(); break;
                default:
                    Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] unknown cmd '{cmd}'");
                    return;
            }
            Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] cmd '{cmd}' -> {(ok ? "ok" : "rejected by app")}");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] cmd error: {ex.GetType().Name}: {ex.Message}");
            manager = null;
        }
    }
}
