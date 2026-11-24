// Sketch del host C# para Media Library (bindings pendientes).
// No compila todavía: la ABI se expone cuando exista webnative.dll y los
// bindings generados. Sirve para ver la separación UI / lógica.

using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;

internal static class Program
{
    [StructLayout(LayoutKind.Sequential)]
    private struct WnConfig
    {
        public IntPtr title;
        public int width;
        public int height;
        public float dpiScale;
        public uint flags;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WnEvent
    {
        public int type;
        public IntPtr targetId;
        public float x, y;
        public int key;
        public uint codepoint;
        public int width, height;
        public uint modifiers;
        public float deltaX, deltaY; // ABI v2 (wheel)
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void WnEventCallback(IntPtr userdata, IntPtr ev);

    [DllImport("webnative", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr wn_app_create(ref WnConfig config);

    [DllImport("webnative", CallingConvention = CallingConvention.Cdecl)]
    private static extern int wn_app_load(IntPtr app, IntPtr html, IntPtr baseUrl, IntPtr css);

    [DllImport("webnative", CallingConvention = CallingConvention.Cdecl)]
    private static extern int wn_app_set_text(IntPtr app, IntPtr id, IntPtr text);

    [DllImport("webnative", CallingConvention = CallingConvention.Cdecl)]
    private static extern int wn_app_set_class(IntPtr app, IntPtr id, IntPtr cls, int enabled);

    [DllImport("webnative", CallingConvention = CallingConvention.Cdecl)]
    private static extern int wn_app_on_event(IntPtr app, WnEventCallback cb, IntPtr userdata);

    [DllImport("webnative", CallingConvention = CallingConvention.Cdecl)]
    private static extern int wn_app_run(IntPtr app);

    [DllImport("webnative", CallingConvention = CallingConvention.Cdecl)]
    private static extern void wn_app_destroy(IntPtr app);

    private static IntPtr _app;
    private static Catalog _catalog = new();

    private static void Main()
    {
        var root = AppContext.BaseDirectory;
        var html = ReadText(Path.Combine(root, "app", "index.html"));
        var css = ReadText(Path.Combine(root, "app", "styles.css"));
        _catalog = JsonSerializer.Deserialize<Catalog>(ReadText(Path.Combine(root, "app", "catalog.json")))!;

        var title = Marshal.StringToHGlobalAnsi("Media Library");
        var cfg = new WnConfig { title = title, width = 1280, height = 800 };
        _app = wn_app_create(ref cfg);
        if (_app == IntPtr.Zero)
        {
            Console.Error.WriteLine("wn_app_create failed");
            return;
        }

        wn_app_load(_app, ToPtr(html), IntPtr.Zero, ToPtr(css));
        wn_app_on_event(_app, OnEvent, IntPtr.Zero);
        wn_app_run(_app);
        wn_app_destroy(_app);
        Marshal.FreeHGlobal(title);
    }

    private static void OnEvent(IntPtr userdata, IntPtr evPtr)
    {
        var ev = Marshal.PtrToStructure<WnEvent>(evPtr);
        if (ev.type != 2) // WN_EVENT_POINTER_UP (v0)
            return;

        var target = Marshal.PtrToStringAnsi(ev.targetId);
        if (target is null) return;

        // El host decide qué significa seleccionar; el engine solo cambia clases.
        // Aquí iría: buscar el item del catálogo, actualizar selección, etc.
        Console.WriteLine($"selected: {target}");
    }

    private static string ReadText(string path) => File.ReadAllText(path);
    private static IntPtr ToPtr(string s) => Marshal.StringToHGlobalAnsi(s);
}

internal sealed class Catalog
{
    public string library { get; set; } = "";
    public Item[] items { get; set; } = Array.Empty<Item>();
}

internal sealed class Item
{
    public string id { get; set; } = "";
    public string title { get; set; } = "";
    public string path { get; set; } = "";
    public string poster { get; set; } = "";
    public string[] tags { get; set; } = Array.Empty<string>();
    public bool favorite { get; set; }
}
