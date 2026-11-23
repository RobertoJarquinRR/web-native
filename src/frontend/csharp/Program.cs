using System.Diagnostics;
using System.Globalization;
using System.Text;
using AngleSharp;
using AngleSharp.Dom;
using AngleSharp.Html.Parser;

namespace WebNative.Frontend;

/// <summary>
/// frontend-csharp: HTML+CSS -> display-list IR (rect-only v1 subset).
/// CLI: --html &lt;path&gt; --width &lt;W&gt; --height &lt;H&gt; --out &lt;ir.json&gt; [--timings &lt;timings.json&gt;]
/// No browser, no WebView, no async pipeline.
/// </summary>
public static class Program
{
    private const string Usage =
        "usage: frontend-csharp --html <path> --width <W> --height <H> --out <ir.json> [--timings <timings.json>]";

    public static int Main(string[] args)
    {
        string? htmlPath = null;
        string? outPath = null;
        string? timingsPath = null;
        int? width = null;
        int? height = null;

        for (var i = 0; i < args.Length; i++)
        {
            var arg = args[i];
            switch (arg)
            {
                case "--html":
                case "--width":
                case "--height":
                case "--out":
                case "--timings":
                    if (i + 1 >= args.Length)
                    {
                        return Fail($"missing value for {arg}");
                    }

                    var value = args[++i];
                    switch (arg)
                    {
                        case "--html": htmlPath = value; break;
                        case "--width":
                            try { width = ParseInt(value, arg); }
                            catch (ArgumentException exception) { return Fail(exception.Message); }
                            break;
                        case "--height":
                            try { height = ParseInt(value, arg); }
                            catch (ArgumentException exception) { return Fail(exception.Message); }
                            break;
                        case "--out": outPath = value; break;
                        case "--timings": timingsPath = value; break;
                    }
                    break;
                case "-h":
                case "--help":
                    Console.WriteLine(Usage);
                    return 0;
                default:
                    return Fail($"unknown argument: {arg}");
            }
        }

        if (htmlPath is null || outPath is null || width is null || height is null)
        {
            return Fail(Usage);
        }

        try
        {
            Run(htmlPath, width.Value, height.Value, outPath, timingsPath);
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"ERROR: {exception.GetType().Name}: {exception.Message}");
            return 1;
        }
    }

    private static void Run(string htmlPath, int viewportWidth, int viewportHeight, string outPath, string? timingsPath)
    {
        var total = Stopwatch.StartNew();
        var timings = new (string Name, double Milliseconds)[6];
        var stage = Stopwatch.StartNew();

        var fullHtmlPath = Path.GetFullPath(htmlPath);
        var html = File.ReadAllText(fullHtmlPath, new UTF8Encoding(false));
        var context = BrowsingContext.New(Configuration.Default);
        var parser = new HtmlParser(new HtmlParserOptions(), context);
        var document = parser.ParseDocument(html);
        var root = document.DocumentElement
            ?? throw new InvalidOperationException("document has no root element");

        var stylesheetTexts = CollectStylesheets(root, Path.GetDirectoryName(fullHtmlPath)!);
        timings[0] = ("parse", Elapsed(stage));

        stage.Restart();
        var rules = CssStyles.ParseRules(stylesheetTexts);
        timings[1] = ("css", Elapsed(stage));

        stage.Restart();
        var styles = Cascade.Compute(root, rules);
        timings[2] = ("cascade", Elapsed(stage));

        stage.Restart();
        var box = Layout.LayoutTree(root, styles, viewportWidth);
        timings[3] = ("layout", Elapsed(stage));

        stage.Restart();
        var ops = Layout.Paint(box);
        var display = new DisplayList { Width = viewportWidth, Height = viewportHeight, Ops = ops };
        var json = display.ToJson();
        WriteFile(outPath, json);
        timings[4] = ("emit", Elapsed(stage));

        var pipeline = Elapsed(total);
        timings[5] = ("total", pipeline);

        var timingsJson = BuildTimingsJson(timings, ops.Count, indented: true);
        if (!string.IsNullOrEmpty(timingsPath))
        {
            WriteFile(timingsPath, timingsJson + "\n");
        }

        Console.WriteLine(BuildTimingsJson(timings, ops.Count, indented: false));
    }

    private static List<string> CollectStylesheets(IElement root, string baseDirectory)
    {
        var texts = new List<string>();
        Walk(root, element =>
        {
            var tag = HtmlDom.TagOf(element);
            if (tag == "style")
            {
                texts.Add(element.TextContent);
            }
            else if (tag == "link")
            {
                var href = element.GetAttribute("href") ?? string.Empty;
                var rel = (element.GetAttribute("rel") ?? string.Empty)
                    .ToLowerInvariant()
                    .Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
                var isStylesheet = Array.IndexOf(rel, "stylesheet") >= 0;
                if (!isStylesheet || href.Length == 0 ||
                    href.StartsWith("http:", StringComparison.OrdinalIgnoreCase) ||
                    href.StartsWith("https:", StringComparison.OrdinalIgnoreCase) ||
                    href.StartsWith("//", StringComparison.Ordinal))
                {
                    return;
                }

                var path = Path.GetFullPath(Path.Combine(baseDirectory, href));
                if (!File.Exists(path))
                {
                    throw new FileNotFoundException($"stylesheet no encontrado: {path}", path);
                }

                texts.Add(File.ReadAllText(path, new UTF8Encoding(false)));
            }
        });
        return texts;
    }

    private static void Walk(IElement element, Action<IElement> visit)
    {
        visit(element);
        foreach (var child in element.Children)
        {
            Walk(child, visit);
        }
    }

    private static string BuildTimingsJson((string Name, double Milliseconds)[] timings, int ops, bool indented)
    {
        var builder = new StringBuilder();
        builder.Append('{');
        if (indented)
        {
            builder.Append('\n');
        }

        for (var i = 0; i < timings.Length; i++)
        {
            if (indented)
            {
                builder.Append("  ");
            }

            builder.Append('"').Append(timings[i].Name).Append("\": ")
                .Append(Json.Number(Math.Round(timings[i].Milliseconds, 3)));
            if (indented)
            {
                builder.Append(",\n");
            }
            else
            {
                builder.Append(", ");
            }
        }

        if (indented)
        {
            builder.Append("  ");
        }

        builder.Append("\"ops\": ").Append(ops.ToString(CultureInfo.InvariantCulture));
        if (indented)
        {
            builder.Append(",\n  ");
        }
        else
        {
            builder.Append(", ");
        }
        builder.Append("\"peak_rss_kb\": ").Append(PeakRssKb().ToString(CultureInfo.InvariantCulture));
        builder.Append(indented ? "\n}" : "}");
        return builder.ToString();
    }

    /// <summary>Optional timing field from SPEC.md; peak working set of this process.</summary>
    private static long PeakRssKb()
    {
        try
        {
            using var process = Process.GetCurrentProcess();
            return process.PeakWorkingSet64 / 1024;
        }
        catch (Exception)
        {
            return 0;
        }
    }

    private static double Elapsed(Stopwatch watch) => watch.Elapsed.TotalMilliseconds;

    private static void WriteFile(string path, string content)
    {
        var directory = Path.GetDirectoryName(Path.GetFullPath(path));
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        // Python's Path.write_text() opens in text mode and therefore emits CRLF on
        // Windows; mirror it so the IR is byte-identical to the reference on this OS.
        var normalized = content.Replace("\r\n", "\n").Replace("\n", "\r\n");
        File.WriteAllText(path, normalized, new UTF8Encoding(false));
    }

    private static int ParseInt(string value, string name) =>
        int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed)
            ? parsed
            : throw new ArgumentException($"invalid integer for {name}: {value}");

    private static int Fail(string message)
    {
        Console.Error.WriteLine($"ERROR: {message}");
        return 2;
    }
}
