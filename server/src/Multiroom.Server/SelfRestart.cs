using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Multiroom.Server;

// The server started again after a server update replaced its files, once the
// host has stopped. Linux replaces this process (execv), so the PID stays and
// systemd, an init script's PID file and the bundle's start.sh still hold it;
// Windows starts a new process on the same console and this one exits.
internal static partial class SelfRestart
{
    public static int Run(string? executable, string[] args)
    {
        if (executable is null)
        {
            Console.Error.WriteLine($"{Branding.ServerName}: restart: the executable is not known");
            return 1;
        }
        if (OperatingSystem.IsWindows())
        {
            var start = new ProcessStartInfo(executable)
            {
                UseShellExecute = false,
                WorkingDirectory = Environment.CurrentDirectory,
            };
            foreach (var argument in args)
                start.ArgumentList.Add(argument);
            using var process = Process.Start(start);
            return 0;
        }
        // Only returns when it failed; the strings then no longer matter.
        IntPtr[] argv = [.. new[] { executable }.Concat(args).Select(Marshal.StringToCoTaskMemUTF8), IntPtr.Zero];
        var failed = Execv(argv[0], argv);
        Console.Error.WriteLine(
            $"{Branding.ServerName}: restart: cannot run {executable}: execv {failed}, error {Marshal.GetLastPInvokeError()}");
        return 1;
    }

    [DllImport("libc", EntryPoint = "execv", SetLastError = true)]
    [DefaultDllImportSearchPaths(DllImportSearchPath.SafeDirectories)]
    private static extern int Execv(IntPtr path, IntPtr[] argv);
}
