using Multiroom.Server;

// Read before anything else: Linux resolves it from /proc/self/exe on first
// use, which follows the running file to .update/previous once a server
// update has swapped the files (SelfRestart).
var executable = Environment.ProcessPath;
if (args is ["--version"])
{
    // A server update checks with it that a new server starts on this machine.
    Console.WriteLine($"{Multiroom.Branding.ServerName} {ServerHost.Version}");
    return 0;
}

var builder = ServerHost.CreateBuilder(args, Console.Error);
if (!builder.IsSuccess)
{
    // No logger exists yet: the failure may be the logger's own file.
    await Console.Error.WriteLineAsync($"{Multiroom.Branding.ServerName}: {builder.Failure}").ConfigureAwait(false);
    return 2;
}
var exitCode = await ServerHost.RunAsync(builder.Value, CancellationToken.None).ConfigureAwait(false);
return exitCode == ServerHost.RestartExitCode ? SelfRestart.Run(executable, args) : exitCode;
