using System.Globalization;
using AngleSharp.Dom;

namespace WebNative.Frontend;

/// <summary>Computed style of one element (v1 subset used by layout and paint).</summary>
public sealed class ComputedStyle
{
    public required Dictionary<string, string> Raw { get; init; }
    public required string Display { get; init; }
    public required double MarginTop { get; init; }
    public required double MarginRight { get; init; }
    public required double MarginBottom { get; init; }
    public required double MarginLeft { get; init; }
    public required double PaddingTop { get; init; }
    public required double PaddingRight { get; init; }
    public required double PaddingBottom { get; init; }
    public required double PaddingLeft { get; init; }
    public required double? Width { get; init; }
    public required double? Height { get; init; }
    public required string? Background { get; init; }
    public required string Color { get; init; }
    public required double FontSize { get; init; }
    public required string FontFamily { get; init; }
    public required string FlexDirection { get; init; }
    public required string JustifyContent { get; init; }
    public required string AlignItems { get; init; }
    public required double Gap { get; init; }
    public required string Text { get; init; }
}

/// <summary>
/// Cascade: selector matching (AngleSharp <c>QuerySelectorAll</c> per rule) plus the
/// reference's priority tuple (important, origin, specificity, selector order,
/// declaration index) and inheritance of the CSS inherited properties.
/// </summary>
public static class Cascade
{
    private static readonly HashSet<string> NonRendered = new(StringComparer.Ordinal)
    {
        "head", "title", "meta", "link", "style", "script", "base", "template",
        "noscript", "source", "track",
    };

    private static readonly HashSet<string> InlineTags = new(StringComparer.Ordinal)
    {
        "a", "abbr", "b", "bdi", "bdo", "br", "cite", "code", "em", "i", "img",
        "label", "q", "s", "small", "span", "strong", "sub", "sup", "u", "var", "wbr",
    };

    private static readonly HashSet<string> InheritedProperties =
        new(CssStyles.Inherited, StringComparer.Ordinal);

    public static Dictionary<IElement, ComputedStyle> Compute(IElement root, List<CssRule> rules)
    {
        var document = root.Owner!;
        foreach (var rule in rules)
        {
            try
            {
                var matched = document.QuerySelectorAll(rule.SelectorText);
                rule.Matches = new HashSet<IElement>(matched, ReferenceEqualityComparer.Instance);
            }
            catch (Exception)
            {
                // Unknown/invalid selector: never matches (does not abort the build).
                rule.Matches = new HashSet<IElement>(ReferenceEqualityComparer.Instance);
            }
        }

        var styles = new Dictionary<IElement, ComputedStyle>(ReferenceEqualityComparer.Instance);
        var elements = new List<IElement>();
        Collect(root, elements);

        foreach (var element in elements)
        {
            var parent = element.ParentElement;
            var raw = new Dictionary<string, string>(StringComparer.Ordinal);
            if (parent is not null && styles.TryGetValue(parent, out var parentStyle))
            {
                foreach (var pair in parentStyle.Raw)
                {
                    if (InheritedProperties.Contains(pair.Key))
                    {
                        raw[pair.Key] = pair.Value;
                    }
                }
            }

            var winners = new Dictionary<string, (PriorityKey Key, string Value)>(StringComparer.Ordinal);
            foreach (var rule in rules)
            {
                if (rule.Matches is null || !rule.Matches.Contains(element))
                {
                    continue;
                }

                var declarations = rule.Declarations;
                for (var i = 0; i < declarations.Count; i++)
                {
                    var declaration = declarations[i];
                    Offer(winners, declaration.Name, declaration.Value, new PriorityKey(
                        declaration.Important,
                        origin: 0,
                        rule.Specificity.Ids,
                        rule.Specificity.Classes,
                        rule.Specificity.Types,
                        rule.Order,
                        i));
                }
            }

            var inline = element.GetAttribute("style");
            if (!string.IsNullOrWhiteSpace(inline))
            {
                var inlineDeclarations = CssStyles.ParseInline(inline);
                for (var i = 0; i < inlineDeclarations.Count; i++)
                {
                    var declaration = inlineDeclarations[i];
                    Offer(winners, declaration.Name, declaration.Value, new PriorityKey(
                        declaration.Important,
                        origin: 1,
                        0, 0, 0,
                        order: 0,
                        i));
                }
            }

            foreach (var pair in winners)
            {
                raw[pair.Key] = pair.Value.Value;
            }

            styles[element] = Resolve(element, raw);
        }

        return styles;
    }

    private static void Offer(
        Dictionary<string, (PriorityKey Key, string Value)> winners,
        string name,
        string value,
        PriorityKey key)
    {
        if (!winners.TryGetValue(name, out var current) || key.CompareTo(current.Key) >= 0)
        {
            winners[name] = (key, value);
        }
    }

    private static ComputedStyle Resolve(IElement element, Dictionary<string, string> raw)
    {
        var tag = HtmlDom.TagOf(element);
        var display = (Get(raw, "display") ?? UserAgentDisplay(tag)).Trim().ToLowerInvariant();
        var fontSize = CssStyles.Length(Get(raw, "font-size"));

        double gap;
        var directGap = CssStyles.Length(Get(raw, "gap"));
        if (directGap is not null)
        {
            gap = directGap.Value;
        }
        else
        {
            var rowGap = CssStyles.Length(Get(raw, "row-gap"));
            var columnGap = CssStyles.Length(Get(raw, "column-gap"));
            // Mirrors Python's `row_gap or column_gap or 0.0` (0.0 is falsy there).
            gap = rowGap is not null and not 0.0 ? rowGap.Value
                : columnGap is not null and not 0.0 ? columnGap.Value
                : 0.0;
        }

        return new ComputedStyle
        {
            Raw = raw,
            Display = display,
            MarginTop = Side(raw, "margin", "top"),
            MarginRight = Side(raw, "margin", "right"),
            MarginBottom = Side(raw, "margin", "bottom"),
            MarginLeft = Side(raw, "margin", "left"),
            PaddingTop = Side(raw, "padding", "top"),
            PaddingRight = Side(raw, "padding", "right"),
            PaddingBottom = Side(raw, "padding", "bottom"),
            PaddingLeft = Side(raw, "padding", "left"),
            Width = CssStyles.Length(Get(raw, "width")),
            Height = CssStyles.Length(Get(raw, "height")),
            Background = CssStyles.ParseColor(Get(raw, "background-color") ?? Get(raw, "background")),
            Color = CssStyles.ParseColor(Get(raw, "color")) ?? "#000000",
            FontSize = fontSize ?? 16.0,
            FontFamily = Get(raw, "font-family") ?? "sans-serif",
            FlexDirection = Keyword(raw, "flex-direction", "row"),
            JustifyContent = Keyword(raw, "justify-content", "flex-start"),
            AlignItems = Keyword(raw, "align-items", "stretch"),
            Gap = gap,
            Text = HtmlDom.OwnText(element),
        };
    }

    private static double Side(Dictionary<string, string> raw, string property, string side) =>
        CssStyles.Length(Get(raw, $"{property}-{side}")) ?? 0.0;

    private static string? Get(Dictionary<string, string> raw, string name) =>
        raw.TryGetValue(name, out var value) ? value : null;

    private static string Keyword(Dictionary<string, string> raw, string name, string fallback)
    {
        var value = Get(raw, name);
        return string.IsNullOrWhiteSpace(value) ? fallback : value.Trim().ToLowerInvariant();
    }

    private static string UserAgentDisplay(string tag)
    {
        if (NonRendered.Contains(tag))
        {
            return "none";
        }

        return InlineTags.Contains(tag) ? "inline" : "block";
    }

    private static void Collect(IElement element, List<IElement> sink)
    {
        sink.Add(element);
        foreach (var child in element.Children)
        {
            Collect(child, sink);
        }
    }
}

/// <summary>
/// Python cascade tuple: (important, origin, specificity(ids, classes, types),
/// selector order, declaration index), compared lexicographically.
/// </summary>
public readonly struct PriorityKey : IComparable<PriorityKey>
{
    private readonly bool _important;
    private readonly int _origin;
    private readonly int _ids;
    private readonly int _classes;
    private readonly int _types;
    private readonly int _order;
    private readonly int _declarationIndex;

    public PriorityKey(bool important, int origin, int ids, int classes, int types, int order, int declarationIndex)
    {
        _important = important;
        _origin = origin;
        _ids = ids;
        _classes = classes;
        _types = types;
        _order = order;
        _declarationIndex = declarationIndex;
    }

    public int CompareTo(PriorityKey other)
    {
        var result = _important.CompareTo(other._important);
        if (result != 0) return result;
        result = _origin.CompareTo(other._origin);
        if (result != 0) return result;
        result = _ids.CompareTo(other._ids);
        if (result != 0) return result;
        result = _classes.CompareTo(other._classes);
        if (result != 0) return result;
        result = _types.CompareTo(other._types);
        if (result != 0) return result;
        result = _order.CompareTo(other._order);
        if (result != 0) return result;
        return _declarationIndex.CompareTo(other._declarationIndex);
    }
}

/// <summary>Small DOM helpers over AngleSharp.</summary>
public static class HtmlDom
{
    public static string TagOf(IElement element) => element.LocalName.ToLowerInvariant();

    /// <summary>Text before the first child element, whitespace-collapsed (empty when blank).</summary>
    public static string OwnText(IElement element)
    {
        var builder = new System.Text.StringBuilder();
        foreach (var node in element.ChildNodes)
        {
            if (node.NodeType == NodeType.Element)
            {
                break;
            }

            if (node.NodeType == NodeType.Text && node.NodeValue is { } text)
            {
                builder.Append(text);
            }
        }

        var raw = builder.ToString();
        if (string.IsNullOrWhiteSpace(raw))
        {
            return string.Empty;
        }

        return string.Join(' ', raw.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries));
    }

    public static IElement? FindBody(IElement root)
    {
        if (TagOf(root) == "body")
        {
            return root;
        }

        foreach (var child in root.Children)
        {
            var found = FindBody(child);
            if (found is not null)
            {
                return found;
            }
        }

        return null;
    }

    public static List<IElement> RenderableChildren(IElement element, Dictionary<IElement, ComputedStyle> styles)
    {
        var result = new List<IElement>();
        foreach (var child in element.Children)
        {
            if (styles.TryGetValue(child, out var style) && (style.Display == "block" || style.Display == "flex"))
            {
                result.Add(child);
            }
        }

        return result;
    }
}
