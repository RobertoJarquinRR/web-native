using System.Globalization;
using System.Text.RegularExpressions;
using AngleSharp.Css.Dom;
using AngleSharp.Css.Parser;
using AngleSharp.Dom;

namespace WebNative.Frontend;

/// <summary>One declaration after shorthand expansion (margin/padding longhands).</summary>
public sealed class CssDeclaration
{
    public required string Name { get; init; }
    public required string Value { get; init; }
    public required bool Important { get; init; }
}

/// <summary>A single (comma-expanded) selector of a qualified rule.</summary>
public sealed class CssRule
{
    public required string SelectorText { get; init; }
    public required (int Ids, int Classes, int Types) Specificity { get; init; }
    public required int Order { get; init; }
    public required List<CssDeclaration> Declarations { get; init; }
    public HashSet<IElement>? Matches { get; set; }
}

/// <summary>
/// CSS parsing on top of AngleSharp.Css: the parser produces a stylesheet whose
/// qualified rules carry <c>SelectorText</c> and a CSSOM declaration block. AngleSharp
/// expands shorthands (margin/padding/background/gap) into longhands, which is what the
/// reference cascade expects after its own expansion. Only the first round's subset is
/// consumed by the cascade; everything else is parsed and ignored.
/// </summary>
public static class CssStyles
{
    private static readonly Regex LengthPattern = new(
        @"(-?(?:\d+\.?\d*|\.\d+))(px)?\z", RegexOptions.CultureInvariant);

    private static readonly HashSet<string> LegacyPseudoElements = new(StringComparer.Ordinal)
    {
        "before", "after", "first-line", "first-letter",
    };

    private static readonly string[] InheritedProperties =
    {
        "color", "font-family", "font-size", "font-weight", "line-height",
        "text-align", "white-space",
    };

    public static IReadOnlyList<string> Inherited => InheritedProperties;

    /// <summary>Parses stylesheet texts into expanded rules, in document order.</summary>
    public static List<CssRule> ParseRules(IEnumerable<string> stylesheetTexts)
    {
        var options = new CssParserOptions
        {
            IsToleratingInvalidSelectors = true,
            IsIncludingUnknownDeclarations = false,
        };
        var parser = new CssParser(options);
        var rules = new List<CssRule>();
        var order = 0;

        foreach (var text in stylesheetTexts)
        {
            if (string.IsNullOrWhiteSpace(text))
            {
                continue;
            }

            ICssStyleSheet sheet;
            try
            {
                sheet = parser.ParseStyleSheet(text);
            }
            catch (Exception)
            {
                // Outside the subset: a malformed sheet must not abort the build.
                continue;
            }

            foreach (var item in sheet.Rules)
            {
                // @media and other at-rules are not part of the subset: skip them
                // exactly like the reference (it only reads qualified top-level rules).
                if (item is not ICssStyleRule styleRule)
                {
                    continue;
                }

                var declarations = CollectDeclarations(styleRule.Style);
                if (declarations.Count == 0)
                {
                    continue;
                }

                foreach (var selector in SplitSelectorList(styleRule.SelectorText))
                {
                    if (selector.Length == 0 || HasPseudoElement(selector))
                    {
                        continue;
                    }

                    rules.Add(new CssRule
                    {
                        SelectorText = selector,
                        Specificity = SpecificityOf(selector),
                        Order = order++,
                        Declarations = declarations,
                    });
                }
            }
        }

        return rules;
    }

    /// <summary>Parses an inline <c>style="..."</c> attribute into declarations.</summary>
    public static List<CssDeclaration> ParseInline(string text)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            return new List<CssDeclaration>();
        }

        try
        {
            var parser = new CssParser(new CssParserOptions
            {
                IsToleratingInvalidSelectors = true,
                IsIncludingUnknownDeclarations = false,
            });
            var declaration = parser.ParseDeclaration(text);
            return declaration is null
                ? new List<CssDeclaration>()
                : CollectDeclarations(declaration);
        }
        catch (Exception)
        {
            return new List<CssDeclaration>();
        }
    }

    private static List<CssDeclaration> CollectDeclarations(ICssStyleDeclaration style)
    {
        var result = new List<CssDeclaration>();
        foreach (var property in style)
        {
            var name = (property.Name ?? string.Empty).Trim().ToLowerInvariant();
            if (name.Length == 0)
            {
                continue;
            }

            var value = property.Value ?? string.Empty;
            if (value.Trim().Length == 0)
            {
                continue;
            }

            AddExpanded(result, name, value, property.IsImportant);
        }

        return result;
    }

    /// <summary>Same expansion the Python reference applies to margin/padding shorthands.</summary>
    private static void AddExpanded(List<CssDeclaration> sink, string name, string value, bool important)
    {
        if (name != "margin" && name != "padding")
        {
            sink.Add(new CssDeclaration { Name = name, Value = value, Important = important });
            return;
        }

        var parts = value.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length is < 1 or > 4)
        {
            return;
        }

        var top = parts[0];
        var right = parts.Length > 1 ? parts[1] : top;
        var bottom = parts.Length > 2 ? parts[2] : top;
        var left = parts.Length > 3 ? parts[3] : right;
        sink.Add(new CssDeclaration { Name = $"{name}-top", Value = top, Important = important });
        sink.Add(new CssDeclaration { Name = $"{name}-right", Value = right, Important = important });
        sink.Add(new CssDeclaration { Name = $"{name}-bottom", Value = bottom, Important = important });
        sink.Add(new CssDeclaration { Name = $"{name}-left", Value = left, Important = important });
    }

    /// <summary>px or unit-less numbers; auto/%/other units become null (auto in v1).</summary>
    public static double? Length(string? value)
    {
        if (value is null)
        {
            return null;
        }

        var text = value.Trim().ToLowerInvariant();
        if (text.Length == 0 || text is "auto" or "none" or "normal")
        {
            return null;
        }

        var match = LengthPattern.Match(text);
        if (!match.Success)
        {
            return null;
        }

        return double.Parse(match.Groups[1].Value, NumberStyles.Float, CultureInfo.InvariantCulture);
    }

    /// <summary>#rgb/#rrggbb/rgb()/rgba()/hsl()/hsla()/names -> lowercase #rrggbb, alpha 0 -> null.</summary>
    public static string? ParseColor(string? value)
    {
        if (value is null)
        {
            return null;
        }

        var text = value.Trim().ToLowerInvariant();
        if (text.Length == 0)
        {
            return null;
        }

        if (text[0] == '#')
        {
            return ParseHexColor(text);
        }

        if (text.StartsWith("rgba(", StringComparison.Ordinal) || text.StartsWith("rgb(", StringComparison.Ordinal))
        {
            return ParseRgbColor(text);
        }

        if (text.StartsWith("hsla(", StringComparison.Ordinal) || text.StartsWith("hsl(", StringComparison.Ordinal))
        {
            return ParseHslColor(text);
        }

        if (text is "transparent" or "currentcolor" or "inherit" or "initial" or "unset" or "revert" or "none" or "auto" or "normal")
        {
            return null;
        }

        return NamedColors.TryGetValue(text, out var named) ? named : null;
    }

    private static string? ParseHexColor(string text)
    {
        var hex = text.AsSpan(1);
        foreach (var ch in hex)
        {
            if (!Uri.IsHexDigit(ch))
            {
                return null;
            }
        }

        return hex.Length switch
        {
            3 => $"#{hex[0]}{hex[0]}{hex[1]}{hex[1]}{hex[2]}{hex[2]}",
            4 => $"#{hex[0]}{hex[0]}{hex[1]}{hex[1]}{hex[2]}{hex[2]}", // #rgba -> ignore alpha
            6 => text,
            8 => text[..7], // #rrggbbaa -> ignore alpha
            _ => null,
        };
    }

    private static string? ParseRgbColor(string text)
    {
        var open = text.IndexOf('(');
        var close = text.LastIndexOf(')');
        if (open < 0 || close <= open)
        {
            return null;
        }

        var body = text[(open + 1)..close].Trim();
        var parts = SplitColorArguments(body);
        if (parts.Count is < 3 or > 4)
        {
            return null;
        }

        var alpha = 1.0;
        if (parts.Count == 4)
        {
            alpha = ParseAlpha(parts[3]);
        }

        if (alpha <= 0.0)
        {
            return null;
        }

        var r = ParseChannel(parts[0]);
        var g = ParseChannel(parts[1]);
        var b = ParseChannel(parts[2]);
        if (r is null || g is null || b is null)
        {
            return null;
        }

        return ToHex(r.Value, g.Value, b.Value);
    }

    private static string? ParseHslColor(string text)
    {
        var open = text.IndexOf('(');
        var close = text.LastIndexOf(')');
        if (open < 0 || close <= open)
        {
            return null;
        }

        var body = text[(open + 1)..close].Trim();
        var parts = SplitColorArguments(body);
        if (parts.Count is < 3 or > 4)
        {
            return null;
        }

        var alpha = parts.Count == 4 ? ParseAlpha(parts[3]) : 1.0;
        if (alpha <= 0.0)
        {
            return null;
        }

        var hueText = parts[0].TrimEnd('d', 'e', 'g');
        if (!double.TryParse(hueText, NumberStyles.Float, CultureInfo.InvariantCulture, out var hue))
        {
            return null;
        }

        var sat = ParsePercent(parts[1]);
        var light = ParsePercent(parts[2]);
        if (sat is null || light is null)
        {
            return null;
        }

        var (r, g, b) = HslToRgb(hue, sat.Value, light.Value);
        return ToHex(r, g, b);
    }

    private static List<string> SplitColorArguments(string body)
    {
        var parts = new List<string>();
        var current = new System.Text.StringBuilder();
        foreach (var ch in body)
        {
            if (ch is ',' or '/' or ' ')
            {
                if (current.Length > 0)
                {
                    parts.Add(current.ToString());
                    current.Clear();
                }
            }
            else
            {
                current.Append(ch);
            }
        }
        if (current.Length > 0)
        {
            parts.Add(current.ToString());
        }
        return parts;
    }

    private static double ParseAlpha(string text)
    {
        var value = text.Trim();
        if (value.EndsWith('%'))
        {
            return double.TryParse(value[..^1], NumberStyles.Float, CultureInfo.InvariantCulture, out var pct)
                ? pct / 100.0
                : 1.0;
        }

        return double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var alpha)
            ? alpha
            : 1.0;
    }

    private static double? ParseChannel(string text)
    {
        var value = text.Trim();
        if (value.EndsWith('%'))
        {
            return double.TryParse(value[..^1], NumberStyles.Float, CultureInfo.InvariantCulture, out var pct)
                ? Math.Round(pct * 255.0 / 100.0)
                : null;
        }

        return double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var number)
            ? Math.Clamp(number, 0.0, 255.0)
            : null;
    }

    private static double? ParsePercent(string text)
    {
        var value = text.Trim();
        if (value.EndsWith('%') && double.TryParse(value[..^1], NumberStyles.Float, CultureInfo.InvariantCulture, out var pct))
        {
            return Math.Clamp(pct / 100.0, 0.0, 1.0);
        }

        return null;
    }

    private static (double R, double G, double B) HslToRgb(double hue, double saturation, double lightness)
    {
        hue = ((hue % 360.0) + 360.0) % 360.0 / 360.0;
        if (saturation <= 0.0)
        {
            return (lightness, lightness, lightness);
        }

        var q = lightness < 0.5
            ? lightness * (1.0 + saturation)
            : lightness + saturation - lightness * saturation;
        var p = 2.0 * lightness - q;
        double Channel(double t)
        {
            t = ((t % 1.0) + 1.0) % 1.0;
            if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
            if (t < 0.5) return q;
            if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
            return p;
        }

        return (Channel(hue + 1.0 / 3.0), Channel(hue), Channel(hue - 1.0 / 3.0));
    }

    private static string ToHex(double r, double g, double b)
    {
        static int Clamp(double v) => (int)Math.Clamp(Math.Round(v, MidpointRounding.AwayFromZero), 0.0, 255.0);
        return $"#{Clamp(r):x2}{Clamp(g):x2}{Clamp(b):x2}";
    }

    /// <summary>Splits a selector list on top-level commas.</summary>
    public static List<string> SplitSelectorList(string selectorText)
    {
        var result = new List<string>();
        var current = new System.Text.StringBuilder();
        var parens = 0;
        var brackets = 0;
        char quote = '\0';
        foreach (var ch in selectorText)
        {
            if (quote != '\0')
            {
                current.Append(ch);
                if (ch == quote)
                {
                    quote = '\0';
                }
                continue;
            }

            switch (ch)
            {
                case '"':
                case '\'':
                    quote = ch;
                    current.Append(ch);
                    break;
                case '(':
                    parens++;
                    current.Append(ch);
                    break;
                case ')':
                    parens = Math.Max(0, parens - 1);
                    current.Append(ch);
                    break;
                case '[':
                    brackets++;
                    current.Append(ch);
                    break;
                case ']':
                    brackets = Math.Max(0, brackets - 1);
                    current.Append(ch);
                    break;
                case ',' when parens == 0 && brackets == 0:
                    Flush(result, current);
                    break;
                default:
                    current.Append(ch);
                    break;
            }
        }

        Flush(result, current);
        return result;
    }

    private static void Flush(List<string> sink, System.Text.StringBuilder buffer)
    {
        var text = buffer.ToString().Trim();
        buffer.Clear();
        if (text.Length > 0)
        {
            sink.Add(text);
        }
    }

    private static bool HasPseudoElement(string selector)
    {
        var brackets = 0;
        char quote = '\0';
        for (var i = 0; i < selector.Length; i++)
        {
            var ch = selector[i];
            if (quote != '\0')
            {
                if (ch == quote) quote = '\0';
                continue;
            }

            if (ch is '"' or '\'')
            {
                quote = ch;
                continue;
            }

            if (ch == '[') { brackets++; continue; }
            if (ch == ']') { brackets = Math.Max(0, brackets - 1); continue; }
            if (brackets > 0) continue;

            if (ch == ':' && i + 1 < selector.Length && selector[i + 1] == ':')
            {
                return true;
            }

            if (ch == ':')
            {
                var end = SkipIdentifier(selector, i + 1);
                var name = selector[(i + 1)..end].ToLowerInvariant();
                if (LegacyPseudoElements.Contains(name))
                {
                    return true;
                }

                // Skip functional pseudo-class arguments, e.g. :not(.a) or :nth-child(2n+1).
                if (end < selector.Length && selector[end] == '(')
                {
                    end = SkipParentheses(selector, end);
                }

                i = end - 1;
            }
        }

        return false;
    }

    /// <summary>Specificity (ids, classes, types) for the benchmark selector subset.</summary>
    public static (int Ids, int Classes, int Types) SpecificityOf(string selector)
    {
        var ids = 0;
        var classes = 0;
        var types = 0;
        var atCompoundStart = true;
        var brackets = 0;
        char quote = '\0';

        for (var i = 0; i < selector.Length;)
        {
            var ch = selector[i];
            if (quote != '\0')
            {
                if (ch == quote) quote = '\0';
                i++;
                continue;
            }

            if (char.IsWhiteSpace(ch))
            {
                if (brackets == 0) atCompoundStart = true;
                i++;
                continue;
            }

            if (brackets > 0)
            {
                if (ch is '"' or '\'') quote = ch;
                else if (ch == ']') brackets--;
                i++;
                continue;
            }

            switch (ch)
            {
                case '>':
                case '+':
                case '~':
                    atCompoundStart = true;
                    i++;
                    continue;
                case '#':
                    ids++;
                    i = SkipIdentifier(selector, i + 1);
                    atCompoundStart = false;
                    continue;
                case '.':
                    classes++;
                    i = SkipIdentifier(selector, i + 1);
                    atCompoundStart = false;
                    continue;
                case '[':
                    classes++;
                    brackets++;
                    i++;
                    atCompoundStart = false;
                    continue;
                case '*':
                    i++;
                    atCompoundStart = false;
                    continue;
                case ':':
                    if (i + 1 < selector.Length && selector[i + 1] == ':')
                    {
                        i = SkipIdentifier(selector, i + 2);
                        if (i < selector.Length && selector[i] == '(') i = SkipParentheses(selector, i);
                        atCompoundStart = false;
                        continue;
                    }

                    {
                        var end = SkipIdentifier(selector, i + 1);
                        i = end;
                        if (i < selector.Length && selector[i] == '(') i = SkipParentheses(selector, i);
                        classes++;
                        atCompoundStart = false;
                        continue;
                    }
                case '(':
                    i = SkipParentheses(selector, i);
                    atCompoundStart = false;
                    continue;
            }

            if (IsIdentifierStart(ch) || ch == '\\' || ch == '|')
            {
                if (ch != '|' && atCompoundStart)
                {
                    types++;
                }

                i = SkipIdentifier(selector, i + 1);
                atCompoundStart = false;
                continue;
            }

            i++;
        }

        return (ids, classes, types);
    }

    private static bool IsIdentifierStart(char ch) =>
        ch == '_' || ch == '-' || char.IsLetter(ch) || ch >= 0x80;

    private static int SkipIdentifier(string text, int index)
    {
        while (index < text.Length)
        {
            var ch = text[index];
            if (ch == '\\' && index + 1 < text.Length)
            {
                index += 2;
                continue;
            }

            if (ch == '_' || ch == '-' || char.IsLetterOrDigit(ch) || ch >= 0x80)
            {
                index++;
                continue;
            }

            break;
        }

        return index;
    }

    private static int SkipParentheses(string text, int index)
    {
        var depth = 0;
        for (var i = index; i < text.Length; i++)
        {
            if (text[i] == '(') depth++;
            else if (text[i] == ')')
            {
                depth--;
                if (depth == 0) return i + 1;
            }
        }

        return text.Length;
    }

    private static readonly Dictionary<string, string> NamedColors = new(StringComparer.Ordinal)
    {
        ["black"] = "#000000", ["silver"] = "#c0c0c0", ["gray"] = "#808080", ["grey"] = "#808080",
        ["white"] = "#ffffff", ["maroon"] = "#800000", ["red"] = "#ff0000", ["purple"] = "#800080",
        ["fuchsia"] = "#ff00ff", ["green"] = "#008000", ["lime"] = "#00ff00", ["olive"] = "#808000",
        ["yellow"] = "#ffff00", ["navy"] = "#000080", ["blue"] = "#0000ff", ["teal"] = "#008080",
        ["aqua"] = "#00ffff", ["cyan"] = "#00ffff", ["magenta"] = "#ff00ff", ["orange"] = "#ffa500",
        ["darkgray"] = "#a9a9a9", ["darkgrey"] = "#a9a9a9", ["lightgray"] = "#d3d3d3",
        ["lightgrey"] = "#d3d3d3", ["dimgray"] = "#696969", ["dimgrey"] = "#696969",
        ["slategray"] = "#708090", ["slategrey"] = "#708090", ["whitesmoke"] = "#f5f5f5",
        ["gainsboro"] = "#dcdcdc", ["rebeccapurple"] = "#663399",
        ["tomato"] = "#ff6347", ["coral"] = "#ff7f50", ["salmon"] = "#fa8072",
        ["gold"] = "#ffd700", ["khaki"] = "#f0e68c", ["indigo"] = "#4b0082",
        ["violet"] = "#ee82ee", ["plum"] = "#dda0dd", ["orchid"] = "#da70d6",
        ["crimson"] = "#dc143c", ["firebrick"] = "#b22222", ["brown"] = "#a52a2a",
        ["chocolate"] = "#d2691e", ["peru"] = "#cd853f", ["tan"] = "#d2b48c",
        ["beige"] = "#f5f5dc", ["ivory"] = "#fffff0", ["snow"] = "#fffafa",
        ["mintcream"] = "#f5fffa", ["azure"] = "#f0ffff", ["aliceblue"] = "#f0f8ff",
        ["steelblue"] = "#4682b4", ["royalblue"] = "#4169e1", ["dodgerblue"] = "#1e90ff",
        ["deepskyblue"] = "#00bfff", ["skyblue"] = "#87ceeb", ["lightblue"] = "#add8e6",
        ["seagreen"] = "#2e8b57", ["forestgreen"] = "#228b22", ["darkgreen"] = "#006400",
        ["yellowgreen"] = "#9acd32", ["olivedrab"] = "#6b8e23", ["limegreen"] = "#32cd32",
    };
}
