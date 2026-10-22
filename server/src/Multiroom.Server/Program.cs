using Multiroom.Server;

var builder = ServerHost.CreateBuilder(args, Console.Error);
if (!builder.IsSuccess)
{
    // No logger exists yet: the failure may be the logger's own file.
    await Console.Error.WriteLineAsync($"{Multiroom.Branding.ServerName}: {builder.Failure}").ConfigureAwait(false);
    return 2;
}
return await ServerHost.RunAsync(builder.Value, CancellationToken.None).ConfigureAwait(false);
