// Draws the AI Upscaler app icon in the Windows 11 Fluent style and writes it as a multi-size
// .ico (PNG entries) plus a 256 px .png. Each size is drawn from vectors at that size, and the
// smallest sizes leave out detail that would only blur.
//
// The picture: a photo tile with a before/after slider, pixelated on the left and smooth on the
// right, with an AI sparkle on the corner.
//
// Usage (from the repository root): dotnet run --project tools\make-icon -- [output folder] [preview folder]

using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

internal static class Program
{
    private static readonly int[] Sizes = [16, 20, 24, 32, 40, 48, 64, 96, 256];

    [STAThread]
    private static int Main(string[] args)
    {
        var folder = args.Length > 0 ? args[0] : Path.Combine("gui", "Assets");
        Directory.CreateDirectory(folder);

        var images = Sizes.Select(size => (size, png: Render(size))).ToList();
        WriteIco(Path.Combine(folder, "AppIcon.ico"), images);
        File.WriteAllBytes(Path.Combine(folder, "AppIcon.png"), images.Single(i => i.size == 256).png);
        // Optional second argument: a folder for every size as a PNG, to check the small ones.
        if (args.Length > 1)
        {
            Directory.CreateDirectory(args[1]);
            foreach (var (size, png) in images)
            {
                File.WriteAllBytes(Path.Combine(args[1], $"AppIcon-{size}.png"), png);
            }
        }
        Console.WriteLine($"Wrote {Path.Combine(folder, "AppIcon.ico")} ({string.Join(", ", Sizes)} px) and AppIcon.png");
        return 0;
    }

    private static byte[] Render(int size)
    {
        var visual = new DrawingVisual();
        using (var dc = visual.RenderOpen())
        {
            // The design is on a 256-unit grid.
            dc.PushTransform(new ScaleTransform(size / 256.0, size / 256.0));
            Draw(dc, detailed: size >= 40, sparkleDetail: size >= 32);
            dc.Pop();
        }
        var bitmap = new RenderTargetBitmap(size, size, 96, 96, PixelFormats.Pbgra32);
        bitmap.Render(visual);

        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var stream = new MemoryStream();
        encoder.Save(stream);
        return stream.ToArray();
    }

    private static Color Hex(string hex, byte alpha = 255)
    {
        var c = (Color)ColorConverter.ConvertFromString(hex);
        c.A = alpha;
        return c;
    }

    private static void Draw(DrawingContext dc, bool detailed, bool sparkleDetail)
    {
        var tile = new Rect(16, 40, 224, 176);
        const double tileRadius = 40;

        // Soft shadow under the tile, as Fluent icons sit slightly above the surface.
        for (var i = 1; i <= 4; i++)
        {
            var shadow = tile;
            shadow.Offset(0, 2 + i * 1.5);
            shadow.Inflate(i * 0.75, i * 0.75);
            dc.DrawRoundedRectangle(new SolidColorBrush(Hex("#000000", (byte)(26 - i * 5))), null, shadow, tileRadius + i, tileRadius + i);
        }

        // Tile: accent blue to violet, with light from the top.
        var tileBrush = new LinearGradientBrush(Hex("#3B8CFF"), Hex("#6A44F2"), new Point(0, 0), new Point(1, 1));
        dc.DrawRoundedRectangle(tileBrush, null, tile, tileRadius, tileRadius);
        var sheen = new LinearGradientBrush(Hex("#FFFFFF", 64), Hex("#FFFFFF", 0), new Point(0.5, 0), new Point(0.5, 0.55));
        dc.DrawRoundedRectangle(sheen, new Pen(new SolidColorBrush(Hex("#FFFFFF", 40)), 2), tile, tileRadius, tileRadius);

        // Picture inside the tile.
        var picture = new Rect(40, 64, 176, 128);
        const double pictureRadius = 22;
        var pictureBrush = new LinearGradientBrush(Hex("#FFFFFF"), Hex("#DCE8FF"), new Point(0.5, 0), new Point(0.5, 1));
        dc.DrawRoundedRectangle(pictureBrush, null, picture, pictureRadius, pictureRadius);

        dc.PushClip(new RectangleGeometry(picture, pictureRadius, pictureRadius));
        var hills = new LinearGradientBrush(Hex("#4C86F5"), Hex("#2A5BD7"), new Point(0.5, 0), new Point(0.5, 1));
        const double split = 128;
        if (detailed)
        {
            // Sun on the smooth side.
            dc.DrawEllipse(new LinearGradientBrush(Hex("#FFD54A"), Hex("#FFAA00"), 90), null, new Point(172, 94), 14, 14);
            // Left: the same hills as blocky pixels (the low-resolution source).
            const double cell = 11;
            var blocks = new StreamGeometry();
            using (var g = blocks.Open())
            {
                for (var x = picture.Left; x < split; x += cell)
                {
                    var top = Math.Round(HillY(x + cell / 2) / cell) * cell;
                    var width = Math.Min(cell, split - x);
                    g.BeginFigure(new Point(x, top), true, true);
                    g.PolyLineTo([new Point(x + width, top), new Point(x + width, picture.Bottom), new Point(x, picture.Bottom)], true, false);
                }
            }
            dc.DrawGeometry(hills, null, blocks);
            // Right: smooth (the upscaled result).
            dc.DrawGeometry(hills, null, HillGeometry(split, picture.Right, picture.Bottom));
        }
        else
        {
            dc.DrawEllipse(new SolidColorBrush(Hex("#FFB900")), null, new Point(172, 94), 17, 17);
            dc.DrawGeometry(hills, null, HillGeometry(picture.Left, picture.Right, picture.Bottom));
        }
        dc.Pop();

        if (detailed)
        {
            // The before/after slider, as in the studio's side-by-side view.
            var white = new SolidColorBrush(Hex("#FFFFFF"));
            dc.DrawRectangle(white, null, new Rect(split - 2.5, picture.Top, 5, picture.Height));
            dc.DrawEllipse(white, new Pen(new SolidColorBrush(Hex("#3B6FF0")), 4), new Point(split, 128), 12, 12);
        }

        // AI sparkle on the top-right corner.
        var gold = new LinearGradientBrush(Hex("#FFE98A"), Hex("#FFAE00"), new Point(0.2, 0), new Point(0.8, 1));
        var outline = new Pen(new SolidColorBrush(Hex("#C77800", 110)), sparkleDetail ? 2 : 3);
        dc.DrawGeometry(gold, outline, Sparkle(new Point(208, 48), sparkleDetail ? 36 : 44));
        if (sparkleDetail)
        {
            dc.DrawGeometry(gold, outline, Sparkle(new Point(240, 92), 13));
        }
    }

    /// <summary>Top of the two hills at x (grid units).</summary>
    private static double HillY(double x)
    {
        static double Hill(double x, double centre, double width, double height) => height * Math.Exp(-Math.Pow((x - centre) / width, 2));
        return 192 - Math.Max(Hill(x, 112, 44, 84), Hill(x, 186, 30, 52)) - 6;
    }

    private static Geometry HillGeometry(double left, double right, double bottom)
    {
        var geometry = new StreamGeometry();
        using (var g = geometry.Open())
        {
            g.BeginFigure(new Point(left, bottom), true, true);
            var points = new List<Point>();
            for (var x = left; x <= right; x += 2)
            {
                points.Add(new Point(x, HillY(x)));
            }
            points.Add(new Point(right, bottom));
            g.PolyLineTo(points, true, true);
        }
        geometry.Freeze();
        return geometry;
    }

    /// <summary>Four-pointed star with concave sides, centred on <paramref name="c"/>.</summary>
    private static Geometry Sparkle(Point c, double r)
    {
        var geometry = new StreamGeometry();
        using (var g = geometry.Open())
        {
            Point P(double dx, double dy) => new(c.X + dx, c.Y + dy);
            var pinch = r * 0.16;
            g.BeginFigure(P(0, -r), true, true);
            g.QuadraticBezierTo(P(pinch, -pinch), P(r, 0), true, true);
            g.QuadraticBezierTo(P(pinch, pinch), P(0, r), true, true);
            g.QuadraticBezierTo(P(-pinch, pinch), P(-r, 0), true, true);
            g.QuadraticBezierTo(P(-pinch, -pinch), P(0, -r), true, true);
        }
        geometry.Freeze();
        return geometry;
    }

    /// <summary>Writes an .ico whose images are PNGs (supported since Windows Vista).</summary>
    private static void WriteIco(string path, IReadOnlyList<(int size, byte[] png)> images)
    {
        using var file = File.Create(path);
        using var w = new BinaryWriter(file);
        w.Write((ushort)0); // reserved
        w.Write((ushort)1); // type: icon
        w.Write((ushort)images.Count);
        var offset = 6 + 16 * images.Count;
        foreach (var (size, png) in images)
        {
            w.Write((byte)(size >= 256 ? 0 : size)); // 0 means 256
            w.Write((byte)(size >= 256 ? 0 : size));
            w.Write((byte)0); // palette colours
            w.Write((byte)0); // reserved
            w.Write((ushort)1); // colour planes
            w.Write((ushort)32); // bits per pixel
            w.Write(png.Length);
            w.Write(offset);
            offset += png.Length;
        }
        foreach (var (_, png) in images)
        {
            w.Write(png);
        }
    }
}
