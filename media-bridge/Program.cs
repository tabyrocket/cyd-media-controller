using Windows.Media.Control;
using Windows.Storage.Streams;

Console.WriteLine("=== MediaBridge console test ===");
Console.WriteLine("Play something in Spotify / a browser / another player. Ctrl+C to quit.\n");

var manager = await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();

string lastKey = "";
DateTime lastProgressPrint = DateTime.MinValue;

while (true)
{
    try
    {
        var session = manager.GetCurrentSession();

        if (session == null)
        {
            if (lastKey != "NONE")
            {
                Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] No active media session");
                lastKey = "NONE";
            }
        }
        else
        {
            var props = await session.TryGetMediaPropertiesAsync();
            var playback = session.GetPlaybackInfo();
            var timeline = session.GetTimelineProperties();

            string app = session.SourceAppUserModelId;
            string state = playback.PlaybackStatus.ToString();   // Playing, Paused, Stopped, Opened, Closed, Changing

            // Artwork: report whether it exists and how big it is
            string art = "none";
            if (props.Thumbnail != null)
            {
                try
                {
                    using IRandomAccessStreamWithContentType s = await props.Thumbnail.OpenReadAsync();
                    art = $"{s.ContentType}, {s.Size} bytes";
                }
                catch { art = "present but unreadable"; }
            }

            // Position is a snapshot taken at LastUpdatedTime, so estimate the live value
            TimeSpan duration = timeline.EndTime - timeline.StartTime;
            TimeSpan position = timeline.Position - timeline.StartTime;
            if (playback.PlaybackStatus == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing)
                position += DateTimeOffset.Now - timeline.LastUpdatedTime;
            if (position < TimeSpan.Zero) position = TimeSpan.Zero;
            if (duration > TimeSpan.Zero && position > duration) position = duration;

            string key = $"{app}|{props.Title}|{props.Artist}|{props.AlbumTitle}|{state}|{art}";
            if (key != lastKey)
            {
                lastKey = key;
                Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] CHANGE");
                Console.WriteLine($"   app    : {app}");
                Console.WriteLine($"   title  : {props.Title}");
                Console.WriteLine($"   artist : {props.Artist}");
                Console.WriteLine($"   album  : {props.AlbumTitle}");
                Console.WriteLine($"   state  : {state}");
                Console.WriteLine($"   artwork: {art}");
                lastProgressPrint = DateTime.MinValue;
            }

            if (DateTime.Now - lastProgressPrint > TimeSpan.FromSeconds(5))
            {
                lastProgressPrint = DateTime.Now;
                Console.WriteLine($"   progress: {position:mm\\:ss} / {duration:mm\\:ss}  (raw duration {duration.TotalMilliseconds:F0} ms)");
            }
        }
    }
    catch (Exception ex)
    {
        Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] ERROR: {ex.GetType().Name}: {ex.Message}");
    }

    await Task.Delay(1000);
}
