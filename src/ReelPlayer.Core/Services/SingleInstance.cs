using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace ReelPlayer.Core.Services;

/// <summary>
/// Ensures one running instance. A second launch forwards its command line over a named pipe and exits.
/// </summary>
public sealed class SingleInstance : IDisposable
{
    private readonly string _pipeName;
    private readonly Mutex _mutex;
    private readonly CancellationTokenSource _cts = new();

    public SingleInstance(string appId)
    {
        var user = Environment.UserName.Replace('\\', '_');
        _pipeName = $"{appId}-{user}-pipe";
        _mutex = new Mutex(initiallyOwned: true, $@"Local\{appId}-{user}-mutex", out var createdNew);
        IsFirstInstance = createdNew;
    }

    public bool IsFirstInstance { get; }

    /// <summary>Raised on a thread-pool thread with the arguments from another launch.</summary>
    public event Action<string[]>? ArgumentsReceived;

    public void StartListening()
    {
        if (!IsFirstInstance) throw new InvalidOperationException("Only the first instance listens.");
        _ = Task.Run(ListenLoop);
    }

    /// <summary>Sends args to the first instance. Returns false if it could not be reached.</summary>
    public bool Forward(string[] args)
    {
        try
        {
            using var client = new NamedPipeClientStream(".", _pipeName, PipeDirection.Out);
            client.Connect(3000);
            var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(args));
            client.Write(bytes, 0, bytes.Length);
            client.Flush();
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    private async Task ListenLoop()
    {
        while (!_cts.IsCancellationRequested)
        {
            try
            {
                await using var server = new NamedPipeServerStream(_pipeName, PipeDirection.In, 1,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                await server.WaitForConnectionAsync(_cts.Token);
                using var ms = new MemoryStream();
                await server.CopyToAsync(ms, _cts.Token);
                var args = JsonSerializer.Deserialize<string[]>(Encoding.UTF8.GetString(ms.ToArray()));
                if (args is not null) ArgumentsReceived?.Invoke(args);
            }
            catch (OperationCanceledException) { break; }
            catch (Exception) { await Task.Delay(200); }
        }
    }

    public void Dispose()
    {
        _cts.Cancel();
        if (IsFirstInstance)
        {
            try { _mutex.ReleaseMutex(); } catch { /* ignore */ }
        }
        _mutex.Dispose();
    }
}
