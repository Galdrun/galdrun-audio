namespace Multiroom.Server.Tests;

// A fresh directory per test, removed afterwards.
internal sealed class TempDirectory : IDisposable
{
    public TempDirectory()
    {
        Path = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "galdrun-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path);
    }

    public string Path { get; }

    public string Write(string name, string content)
    {
        var file = System.IO.Path.Combine(Path, name);
        File.WriteAllText(file, content);
        return file;
    }

    public void Dispose()
    {
        try
        {
            Directory.Delete(Path, recursive: true);
        }
        catch (IOException)
        {
            // A file still held open is left for the OS's temp cleanup.
        }
    }
}
