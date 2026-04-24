using System.Reflection;
using System.Runtime.InteropServices;

namespace TachyonIpc.Native;

internal static class NativeLoader
{
    private static int _loaded;

    internal static void Register()
    {
        if (Interlocked.CompareExchange(ref _loaded, 1, 0) != 0)
            return;
        NativeLibrary.SetDllImportResolver(typeof(NativeLoader).Assembly, Resolver);
    }

    private static nint Resolver(string name, Assembly asm, DllImportSearchPath? _)
    {
        if (name != "tachyon")
            return nint.Zero;

        var rid = GetRid();
        var ext = RuntimeInformation.IsOSPlatform(OSPlatform.OSX) ? "dylib" : "so";
        var asmDir = Path.GetDirectoryName(asm.Location) ?? AppContext.BaseDirectory;
        var candidate = Path.Combine(asmDir, "runtimes", rid, "native", $"libtachyon.{ext}");
        if (File.Exists(candidate) && NativeLibrary.TryLoad(candidate, out var h1))
            return h1;

        candidate = Path.Combine(AppContext.BaseDirectory, "runtimes", rid, "native", $"libtachyon.{ext}");
        if (File.Exists(candidate) && NativeLibrary.TryLoad(candidate, out var h2))
            return h2;

        return NativeLibrary.TryLoad($"libtachyon.{ext}", out var h3) ? h3 : nint.Zero;
    }

    private static string GetRid() =>
        (RuntimeInformation.ProcessArchitecture, RuntimeInformation.IsOSPlatform(OSPlatform.OSX)) switch
        {
            (Architecture.X64, false) => "linux-x64",
            (Architecture.Arm64, false) => "linux-arm64",
            (Architecture.X64, true) => "osx-x64",
            (Architecture.Arm64, true) => "osx-arm64",
            _ => throw new PlatformNotSupportedException(RuntimeInformation.RuntimeIdentifier)
        };
}