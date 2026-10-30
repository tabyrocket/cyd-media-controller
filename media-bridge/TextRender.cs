using System.Drawing;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Runtime.InteropServices;

// Renders a line of text to an 8-bit alpha mask that the ESP32 blends over its gradient.
//
// Wire format: [w lo][w hi][h lo][h hi] followed by w*h alpha bytes (row-major, 0 = transparent).
//
// The constants below MUST match the ESP32 firmware (IMG_TITLE_*, IMG_SUB_*, TEXT_W).
static class TextRender
{
    public const int TitleH = 28;       // = SPR_H on the ESP32
    public const int SubH = 18;
    public const int TitleMaxW = 800;   // = IMG_TITLE_MAXW (longer titles are cut with an ellipsis)
    public const int SubMaxW = 166;     // = TEXT_W (artist/album must fit without scrolling)

    const float TitleEm = 24f, SubEm = 15f;
    const int TitleBaseline = 21, SubBaseline = 14;

    // Strokes at small sizes come out thin and dim. Alpha is raised to this power for the
    // artist/album lines (1.0 = unchanged, lower = bolder/crisper, try 0.6 - 0.85).
    const double SubGamma = 0.40;
    static readonly byte[] SubLut = BuildLut(SubGamma);
    static byte[] BuildLut(double g)
    {
        var l = new byte[256];
        for (int i = 0; i < 256; i++) l[i] = (byte)Math.Round(255.0 * Math.Pow(i / 255.0, g));
        return l;
    }

    // Anything outside printable ASCII can't be drawn by the built-in fonts.
    public static bool NeedsImage(string s)
    {
        foreach (char c in s) if (c < 32 || c > 126) return true;
        return false;
    }

    static readonly Lazy<HashSet<string>> Installed = new(() =>
    {
        using var col = new InstalledFontCollection();
        return new HashSet<string>(col.Families.Select(f => f.Name), StringComparer.OrdinalIgnoreCase);
    });

    static string First(params string[] names)
    {
        foreach (var n in names) if (Installed.Value.Contains(n)) return n;
        return "Arial";
    }

    static string PickFont(string s)
    {
        bool hangul = false, kana = false, han = false;
        foreach (char c in s)
        {
            if ((c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F) ||
                (c >= 0xA960 && c <= 0xA97F) || (c >= 0xAC00 && c <= 0xD7FF)) hangul = true;
            else if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) ||
                     (c >= 0xFF66 && c <= 0xFF9F)) kana = true;
            else if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) ||
                     (c >= 0xF900 && c <= 0xFAFF)) han = true;
        }
        if (hangul) return First("Malgun Gothic", "Segoe UI");
        if (kana || han) return First("Yu Gothic UI", "Meiryo UI", "MS UI Gothic", "Segoe UI");
        return First("Segoe UI", "Arial");
    }

    // Returns null when the text is empty, plain ASCII (unless force), or rendering fails.
    public static byte[]? Render(string? raw, bool title, bool force = false)
    {
        if (string.IsNullOrEmpty(raw) || (!force && !NeedsImage(raw))) return null;
        try
        {
            string text = new string(raw.Select(c => c < 32 ? ' ' : c).ToArray());
            int h = title ? TitleH : SubH;
            int maxW = title ? TitleMaxW : SubMaxW;
            float em = title ? TitleEm : SubEm;
            int baseline = title ? TitleBaseline : SubBaseline;

            using var family = new FontFamily(PickFont(text));
            using var font = new Font(family, em, FontStyle.Regular, GraphicsUnit.Pixel);
            using var fmt = (StringFormat)StringFormat.GenericTypographic.Clone();
            fmt.FormatFlags |= StringFormatFlags.NoWrap | StringFormatFlags.MeasureTrailingSpaces;

            using var probe = new Bitmap(1, 1);
            using var gm = Graphics.FromImage(probe);
            gm.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            float Measure(string s) => gm.MeasureString(s, font, int.MaxValue, fmt).Width;

            if (Measure(text) > maxW)
            {
                while (text.Length > 1 && Measure(text + "\u2026") > maxW) text = text[..^1];
                if (text.Length > 0 && char.IsHighSurrogate(text[^1])) text = text[..^1];
                text += "\u2026";
            }

            int w = Math.Clamp((int)Math.Ceiling(Measure(text)) + 1, 1, maxW);

            using var bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb);
            using (var g = Graphics.FromImage(bmp))
            {
                g.Clear(Color.Black);
                g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
                float ascent = em * family.GetCellAscent(FontStyle.Regular) / family.GetEmHeight(FontStyle.Regular);
                float top = (float)Math.Round(baseline - ascent);   // whole pixels only, fractional y blurs small text
                g.DrawString(text, font, Brushes.White, new PointF(0, top), fmt);
            }

            var bd = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
            int stride = Math.Abs(bd.Stride);
            var px = new byte[stride * h];
            Marshal.Copy(bd.Scan0, px, 0, px.Length);
            bmp.UnlockBits(bd);

            var res = new byte[4 + w * h];
            res[0] = (byte)w; res[1] = (byte)(w >> 8);
            res[2] = (byte)h; res[3] = (byte)(h >> 8);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    {
                    byte a = px[y * stride + x * 3 + 1];            // green channel = coverage
                    res[4 + y * w + x] = title ? a : SubLut[a];
                }
            return res;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"   text render failed: {ex.GetType().Name}: {ex.Message}");
            return null;
        }
    }

    // Debug helper: turns a mask back into a PNG so you can look at it in a browser.
    public static byte[] MaskToPng(byte[] mask)
    {
        int w = mask[0] | (mask[1] << 8), h = mask[2] | (mask[3] << 8);
        using var bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int a = mask[4 + y * w + x];
                bmp.SetPixel(x, y, Color.FromArgb(a, a, a));
            }
        using var ms = new MemoryStream();
        bmp.Save(ms, ImageFormat.Png);
        return ms.ToArray();
    }
}