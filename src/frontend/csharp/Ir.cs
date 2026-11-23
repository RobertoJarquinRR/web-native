using System.Globalization;
using System.Text;

namespace WebNative.Frontend;

/// <summary>A rect op of the display list (rect-only round of the benchmark).</summary>
public readonly record struct RectOp(double X, double Y, double W, double H, string Fill);

/// <summary>Display list: viewport + ops in painter's order.</summary>
public sealed class DisplayList
{
    public required int Width { get; init; }
    public required int Height { get; init; }
    public required List<RectOp> Ops { get; init; }

    /// <summary>
    /// Serializes the IR exactly like <c>DisplayList.to_json(indent=2)</c> in the
    /// Python reference: geometry is rounded to integers (Python's round()), fills are
    /// lowercase #rrggbb, ops keep pre-order.
    /// </summary>
    public string ToJson()
    {
        var sb = new StringBuilder(256 + Ops.Count * 172);
        sb.Append("{\n  \"viewport\": {\n");
        sb.Append("    \"width\": ").Append(Width.ToString(CultureInfo.InvariantCulture)).Append(",\n");
        sb.Append("    \"height\": ").Append(Height.ToString(CultureInfo.InvariantCulture)).Append("\n  },\n");
        sb.Append("  \"ops\": ");
        if (Ops.Count == 0)
        {
            sb.Append("[]");
        }
        else
        {
            sb.Append("[\n");
            for (var i = 0; i < Ops.Count; i++)
            {
                var op = Ops[i];
                sb.Append("    {\n");
                sb.Append("      \"type\": \"rect\",\n");
                sb.Append("      \"x\": ").Append(Json.Number(Round(op.X))).Append(",\n");
                sb.Append("      \"y\": ").Append(Json.Number(Round(op.Y))).Append(",\n");
                sb.Append("      \"w\": ").Append(Json.Number(Round(op.W))).Append(",\n");
                sb.Append("      \"h\": ").Append(Json.Number(Round(op.H))).Append(",\n");
                sb.Append("      \"fill\": ").Append(Json.String(op.Fill)).Append('\n');
                sb.Append("    }");
                if (i + 1 < Ops.Count)
                {
                    sb.Append(',');
                }
                sb.Append('\n');
            }
            sb.Append("  ]");
        }
        sb.Append("\n}\n");
        return sb.ToString();
    }

    /// <summary>Python's built-in <c>round(float)</c> is half-to-even, same as Math.Round default.</summary>
    private static double Round(double value) => Math.Round(value, MidpointRounding.ToEven);
}
