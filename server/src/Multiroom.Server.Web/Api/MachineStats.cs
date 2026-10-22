using System.Globalization;
using System.Runtime.InteropServices;

namespace Multiroom.Server.Web.Api;

// The machine-wide figures .NET has no portable call for, taken the way the
// core's SystemSampler takes them: GetSystemTimes and GlobalMemoryStatusEx on
// Windows, /proc/stat and /proc/meminfo on Linux. Anywhere else, or when the
// call fails, they are absent.
internal static class MachineStats
{
    // Busy and total CPU time of all cores, in the platform's own ticks; only
    // differences between two readings mean anything.
    public static bool TryCpuTicks(out ulong busy, out ulong total)
    {
        busy = total = 0;
        if (OperatingSystem.IsWindows())
        {
            if (!NativeMethods.GetSystemTimes(out var idle, out var kernel, out var user))
                return false;
            // Kernel time includes idle time.
            total = kernel + user;
            busy = total - Math.Min(total, idle);
            return true;
        }
        if (!OperatingSystem.IsLinux())
            return false;

        try
        {
            using var reader = new StreamReader("/proc/stat");
            var fields = reader.ReadLine()?.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (fields is null || fields.Length < 5 || fields[0] != "cpu")
                return false;
            // user nice system idle iowait irq softirq steal; guest time is in user.
            ulong idleTicks = 0;
            for (var i = 1; i < Math.Min(fields.Length, 9); i++)
            {
                var ticks = ulong.Parse(fields[i], CultureInfo.InvariantCulture);
                total += ticks;
                if (i is 4 or 5)
                    idleTicks += ticks;
            }
            busy = total - idleTicks;
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or FormatException or OverflowException)
        {
            return false;
        }
    }

    public static (ulong Total, ulong Available)? Memory()
    {
        if (OperatingSystem.IsWindows())
        {
            var status = new NativeMethods.MemoryStatusEx { Length = (uint)Marshal.SizeOf<NativeMethods.MemoryStatusEx>() };
            return NativeMethods.GlobalMemoryStatusEx(ref status) ? (status.TotalPhys, status.AvailPhys) : null;
        }
        if (!OperatingSystem.IsLinux())
            return null;

        try
        {
            ulong? total = null, available = null;
            foreach (var line in File.ReadLines("/proc/meminfo"))
            {
                if (line.StartsWith("MemTotal:", StringComparison.Ordinal))
                    total = KibOf(line);
                else if (line.StartsWith("MemAvailable:", StringComparison.Ordinal))
                    available = KibOf(line);
            }
            return total is { } t && available is { } a ? (t, a) : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or FormatException or OverflowException)
        {
            return null;
        }
    }

    // "MemTotal:       16310252 kB"
    private static ulong KibOf(string line) =>
        ulong.Parse(line.Split(' ', StringSplitOptions.RemoveEmptyEntries)[1], CultureInfo.InvariantCulture) * 1024;

    // What the machine is, read once and named as the core's SystemSampler
    // names it: the CPU's architecture, model and cores, and the machine's model.
    public sealed record Identity(string? Architecture, string? CpuModel, string? MachineModel, ulong? Cores);

    private static readonly Lazy<Identity> Machine = new(ReadMachine);

    public static Identity What() => Machine.Value;

    private static Identity ReadMachine()
    {
        var cores = Environment.ProcessorCount > 0 ? (ulong)Environment.ProcessorCount : (ulong?)null;
        if (OperatingSystem.IsWindows())
        {
            return new Identity(ArchitectureName(RuntimeInformation.OSArchitecture),
                RegistryString(@"HARDWARE\DESCRIPTION\System\CentralProcessor\0", "ProcessorNameString"),
                RegistryString(@"HARDWARE\DESCRIPTION\System\BIOS", "SystemProductName"),
                cores);
        }
        if (!OperatingSystem.IsLinux())
            return new Identity(ArchitectureName(RuntimeInformation.OSArchitecture), null, null, cores);

        // uname's machine ("armv7l", "aarch64"), which the kernel also keeps here.
        var architecture = ReadText("/proc/sys/kernel/arch") ?? ArchitectureName(RuntimeInformation.OSArchitecture);
        string? model = null, part = null;
        try
        {
            foreach (var line in File.ReadLines("/proc/cpuinfo"))
            {
                var colon = line.IndexOf(':', StringComparison.Ordinal);
                if (colon < 0)
                    continue;
                var key = line[..colon].Trim();
                var value = line[(colon + 1)..].Trim();
                if (key == "model name" && model is null && value.Length > 0)
                    model = value;
                else if (key == "CPU part" && part is null)
                    part = value;
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
        // 64-bit ARM kernels list no "model name", only the core's part number.
        model ??= part switch
        {
            "0xb76" => "ARM1176",
            "0xc07" => "Cortex-A7",
            "0xd03" => "Cortex-A53",
            "0xd04" => "Cortex-A35",
            "0xd05" => "Cortex-A55",
            "0xd07" => "Cortex-A57",
            "0xd08" => "Cortex-A72",
            "0xd0b" => "Cortex-A76",
            _ => null,
        };
        // Device tree boards (every Raspberry Pi) name themselves here, NUL-terminated.
        return new Identity(architecture, model, ReadText("/proc/device-tree/model"), cores);
    }

    // The clock: the rated one on Windows, the first core's current one on
    // Linux (cpufreq reports kHz; absent without a cpufreq driver).
    public static ulong? CpuMhz()
    {
        if (OperatingSystem.IsWindows())
        {
            return Microsoft.Win32.Registry.GetValue(@"HKEY_LOCAL_MACHINE\HARDWARE\DESCRIPTION\System\CentralProcessor\0",
                "~MHz", null) is int mhz && mhz > 0 ? (ulong)mhz : null;
        }
        if (!OperatingSystem.IsLinux())
            return null;
        return ulong.TryParse(ReadText("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"), NumberStyles.None,
            CultureInfo.InvariantCulture, out var khz) && khz > 0 ? (khz + 500) / 1000 : null;
    }

    private static string? ArchitectureName(Architecture architecture) => architecture switch
    {
        Architecture.X64 => "x86_64",
        Architecture.Arm64 => "aarch64",
        Architecture.X86 => "x86",
        Architecture.Arm => "arm",
        _ => null,
    };

    [System.Runtime.Versioning.SupportedOSPlatform("windows")]
    private static string? RegistryString(string key, string name) =>
        // Intel pads the processor name with spaces on both sides.
        (Microsoft.Win32.Registry.GetValue($@"HKEY_LOCAL_MACHINE\{key}", name, null) as string)?.Trim() is { Length: > 0 } text
            ? text : null;

    private static string? ReadText(string path)
    {
        try
        {
            return File.ReadAllText(path).Trim().TrimEnd('\0').Trim() is { Length: > 0 } text ? text : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    private static class NativeMethods
    {
        [StructLayout(LayoutKind.Sequential)]
        public struct MemoryStatusEx
        {
            public uint Length;
            public uint MemoryLoad;
            public ulong TotalPhys;
            public ulong AvailPhys;
            public ulong TotalPageFile;
            public ulong AvailPageFile;
            public ulong TotalVirtual;
            public ulong AvailVirtual;
            public ulong AvailExtendedVirtual;
        }

        [DllImport("kernel32.dll")]
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetSystemTimes(out ulong idle, out ulong kernel, out ulong user);

        [DllImport("kernel32.dll")]
        [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GlobalMemoryStatusEx(ref MemoryStatusEx status);
    }
}
