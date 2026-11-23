using AngleSharp.Dom;

namespace WebNative.Frontend;

/// <summary>A laid-out box (border-box rect) plus its paint metadata.</summary>
public sealed class LayoutBox
{
    public required IElement Node { get; init; }
    public required ComputedStyle Style { get; init; }
    public double X { get; set; }
    public double Y { get; set; }
    public double Width { get; set; }
    public double Height { get; set; }
    public required string Kind { get; init; }
    public List<LayoutBox> Children { get; } = new();
    public double MarginBottom { get; init; }

    public double Right => X + Width;
    public double Bottom => Y + Height;

    public IEnumerable<LayoutBox> Walk()
    {
        yield return this;
        foreach (var child in Children)
        {
            foreach (var descendant in child.Walk())
            {
                yield return descendant;
            }
        }
    }

    /// <summary>Moves the whole subtree (flex measures at 0,0 and then places it).</summary>
    public void Translate(double dx, double dy)
    {
        foreach (var box in Walk())
        {
            box.X += dx;
            box.Y += dy;
        }
    }
}

/// <summary>
/// Port of the Python v1 layout: block flow, flex row/column with gap,
/// justify-content and align-items, border-box sizing, auto width/height.
/// No margins collapsing, no borders, no wrap, no percentages.
/// </summary>
public static class Layout
{
    public static LayoutBox LayoutTree(
        IElement root,
        Dictionary<IElement, ComputedStyle> styles,
        int viewportWidth)
    {
        var body = HtmlDom.FindBody(root) ?? root;
        var style = styles[body];
        return LayoutBoxOf(body, style, styles, 0.0, 0.0, viewportWidth);
    }

    private static LayoutBox LayoutBoxOf(
        IElement node,
        ComputedStyle style,
        Dictionary<IElement, ComputedStyle> styles,
        double originX,
        double originY,
        double availableWidth)
    {
        var width = style.Width ?? Math.Max(availableWidth - style.MarginLeft - style.MarginRight, 0.0);
        var x = originX + style.MarginLeft;
        var y = originY + style.MarginTop;
        var contentX = x + style.PaddingLeft;
        var contentY = y + style.PaddingTop;
        var contentWidth = Math.Max(width - style.PaddingRight - style.PaddingLeft, 0.0);

        var box = new LayoutBox
        {
            Node = node,
            Style = style,
            X = x,
            Y = y,
            Width = width,
            Height = 0.0,
            Kind = style.Display is "block" or "flex" ? style.Display : "block",
            MarginBottom = style.MarginBottom,
        };

        var cursorY = contentY;
        // Text nodes are out of scope for this benchmark round (both fixtures are
        // rect-only); whitespace-only text is collapsed and ignored by the reference too.

        var children = HtmlDom.RenderableChildren(node, styles);
        if (style.Display == "flex" && children.Count > 0)
        {
            var contentHeight = style.Height is null
                ? (double?)null
                : Math.Max(style.Height.Value - style.PaddingTop - style.PaddingBottom, 0.0);
            cursorY += LayoutFlexChildren(
                box, children, styles, contentX, cursorY, contentWidth, contentHeight, style);
        }
        else
        {
            foreach (var child in children)
            {
                var childStyle = styles[child];
                var childBox = LayoutBoxOf(child, childStyle, styles, contentX, cursorY, contentWidth);
                box.Children.Add(childBox);
                cursorY = childBox.Bottom + childBox.MarginBottom;
            }
        }

        var contentUsed = cursorY - contentY;
        box.Height = style.Height ?? Math.Max(contentUsed + style.PaddingTop + style.PaddingBottom, 0.0);
        return box;
    }

    /// <summary>(start offset, spacing) on the main axis for justify-content.</summary>
    private static (double Start, double Spacing) Justify(string name, double free, int count, double gap)
    {
        if (count <= 1)
        {
            var start = name == "center" ? free / 2.0
                : name is "flex-end" or "end" ? free
                : 0.0;
            return (start, gap);
        }

        return name switch
        {
            "center" => (free / 2.0, gap),
            "flex-end" or "end" => (free, gap),
            "space-between" => (0.0, gap + free / (count - 1)),
            _ => (0.0, gap),
        };
    }

    private static double LayoutFlexChildren(
        LayoutBox parentBox,
        List<IElement> children,
        Dictionary<IElement, ComputedStyle> styles,
        double contentX,
        double contentY,
        double contentWidth,
        double? contentHeight,
        ComputedStyle parent)
    {
        if (children.Count == 0)
        {
            return 0.0;
        }

        var direction = parent.FlexDirection;
        var gap = Math.Max(parent.Gap, 0.0);

        var measured = new List<(IElement Node, ComputedStyle Style, LayoutBox Box)>(children.Count);
        foreach (var child in children)
        {
            var childStyle = styles[child];
            var measuredBox = LayoutBoxOf(child, childStyle, styles, 0.0, 0.0, contentWidth);
            measured.Add((child, childStyle, measuredBox));
        }

        var row = direction != "column"; // any non-column value behaves as row
        var mainSizes = new double[measured.Count];
        var crossSizes = new double[measured.Count];
        for (var i = 0; i < measured.Count; i++)
        {
            mainSizes[i] = row ? measured[i].Box.Width : measured[i].Box.Height;
            crossSizes[i] = row ? measured[i].Box.Height : measured[i].Box.Width;
        }

        double extent;
        double mainAvailable;
        if (row)
        {
            extent = contentHeight ?? (crossSizes.Length > 0 ? crossSizes.Max() : 0.0);
            mainAvailable = contentWidth;
        }
        else
        {
            extent = contentWidth;
            mainAvailable = contentHeight ?? (mainSizes.Sum() + gap * (measured.Count - 1));
        }

        var used = mainSizes.Sum() + gap * (measured.Count - 1);
        var free = Math.Max(mainAvailable - used, 0.0);
        var (start, spacing) = Justify(parent.JustifyContent, free, measured.Count, gap);

        var cursor = 0.0;
        for (var i = 0; i < measured.Count; i++)
        {
            var (_, childStyle, box) = measured[i];
            var mainSize = mainSizes[i];
            var crossSize = crossSizes[i];

            // Cross-axis stretch only when the child did not fix that dimension.
            if (parent.AlignItems is "stretch" or "normal")
            {
                if (row && childStyle.Height is null)
                {
                    box.Height = extent;
                    crossSize = extent;
                }
                else if (!row && childStyle.Width is null)
                {
                    box.Width = extent;
                    crossSize = extent;
                }
            }

            var crossOffset = parent.AlignItems switch
            {
                "center" => (extent - crossSize) / 2.0,
                "flex-end" or "end" => extent - crossSize,
                _ => 0.0, // stretch / flex-start / start
            };

            if (row)
            {
                box.Translate(contentX + start + cursor, contentY + crossOffset);
            }
            else
            {
                box.Translate(contentX + crossOffset, contentY + start + cursor);
            }

            parentBox.Children.Add(box);
            cursor += mainSize + spacing;
        }

        return row ? extent : used;
    }

    /// <summary>Pre-order rects: parent background first, then children in document order.</summary>
    public static List<RectOp> Paint(LayoutBox box)
    {
        var ops = new List<RectOp>();
        PaintInto(box, ops);
        return ops;
    }

    private static void PaintInto(LayoutBox box, List<RectOp> ops)
    {
        if (!string.IsNullOrEmpty(box.Style.Background))
        {
            ops.Add(new RectOp(box.X, box.Y, box.Width, box.Height, box.Style.Background!));
        }

        foreach (var child in box.Children)
        {
            PaintInto(child, ops);
        }
    }
}
