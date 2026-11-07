using System.Net;
using System.Net.Sockets;
using System.Text;
using Arcadia.EA;
using Arcadia.Storage;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;

namespace Arcadia.Hosting;

/// <summary>
/// Player-hosted games. The 360 game never registers the match it hosts with
/// Theater (no CGAM), so the bf1943 runtime does it here instead, over a plain
/// TCP line protocol:
///   REG user=&lt;host gamertag&gt; port=&lt;udp&gt; [int-ip=&lt;lan ip&gt;] [ip=&lt;public ip&gt;] [max=&lt;players&gt;] [name=&lt;text&gt;] [map=&lt;level&gt;]
/// answered with "OK &lt;gid&gt;" or "ERR &lt;reason&gt;". The game is listed (Quick
/// Match, GLST/GDAT) and joinable (EGAM, answered by Arcadia) while the
/// connection stays open and the host is logged in.
/// </summary>
public class ExternalServerHostedService(IOptions<ArcadiaSettings> settings, ILogger<ExternalServerHostedService> logger,
    ConnectionManager connections, SharedCounters counters) : BackgroundService
{
    private readonly ArcadiaSettings _settings = settings.Value;

    protected override async Task ExecuteAsync(CancellationToken ct)
    {
        if (_settings.ExternalServerPort <= 0) return;

        var listener = new TcpListener(IPAddress.Parse(_settings.ListenAddress), _settings.ExternalServerPort);
        listener.Start();
        logger.LogInformation("External game registration on port {port}", _settings.ExternalServerPort);

        while (!ct.IsCancellationRequested)
        {
            var client = await listener.AcceptTcpClientAsync(ct);
            _ = Task.Run(() => HandleClient(client, ct), ct);
        }
    }

    private async Task HandleClient(TcpClient client, CancellationToken ct)
    {
        GameServerListing? game = null;
        var remote = ((IPEndPoint)client.Client.RemoteEndPoint!).Address.MapToIPv4().ToString();

        try
        {
            using var stream = client.GetStream();
            using var reader = new StreamReader(stream, Encoding.ASCII);
            using var writer = new StreamWriter(stream, Encoding.ASCII) { AutoFlush = true, NewLine = "\n" };

            string? line;
            while ((line = await reader.ReadLineAsync(ct)) is not null)
            {
                var parts = line.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length == 0) continue;

                var args = parts.Skip(1).Select(x => x.Split('=', 2)).Where(x => x.Length == 2)
                    .ToDictionary(x => x[0].ToLowerInvariant(), x => Uri.UnescapeDataString(x[1]));

                if (parts[0] != "REG" || game is not null)
                {
                    await writer.WriteLineAsync("ERR bad request");
                    continue;
                }

                var user = args.GetValueOrDefault("user") ?? string.Empty;
                var host = connections.FindSessionByUsername(user);
                if (host is null)
                {
                    await writer.WriteLineAsync("ERR host not logged in");
                    continue;
                }

                if (connections.GetServerByHostPlayer(host.User.UserId) is { } old)
                {
                    await connections.RemoveGameListing(old);
                }

                var address = args.GetValueOrDefault("ip") ?? remote;
                var port = args.GetValueOrDefault("port") ?? "1003";
                var name = args.GetValueOrDefault("name") ?? $"{host.User.Username}'s game";

                game = new GameServerListing
                {
                    External = true,
                    TheaterConnection = host.TheaterConnection,
                    ExternalAddress = address,
                    PartitionId = host.PartitionId,
                    Platform = "xenon",
                    UID = host.User.UserId,
                    GID = counters.GetNextGameId(),
                    LID = 257,
                    UGID = "NOGUID",
                    EKEY = "NOENCYRPTIONKEY",
                    SECRET = "NOSECRET",
                    NAME = name,
                    CanJoin = true,
                    Data = new()
                    {
                        ["NAME"] = name,
                        ["PORT"] = port,
                        ["INT-PORT"] = args.GetValueOrDefault("int-port") ?? port,
                        ["INT-IP"] = args.GetValueOrDefault("int-ip") ?? address,
                        ["MAX-PLAYERS"] = args.GetValueOrDefault("max") ?? "24",
                        ["TYPE"] = "G",
                        ["JOIN"] = "O",
                        ["HTTYPE"] = "A",
                        ["RT"] = "",
                        ["B-version"] = "",
                        ["B-numObservers"] = "0",
                        ["B-maxObservers"] = "0",
                        ["B-U-map"] = args.GetValueOrDefault("map") ?? "",
                        ["TICKET"] = $"{counters.GetNextTicket()}",
                    }
                };

                await connections.AddGameListing(game, new Dictionary<string, string>());
                logger.LogInformation("External game {gid} '{name}' by {user} at {address}:{port} ({partition})",
                    game.GID, name, host.User.Username, address, port, host.PartitionId);
                await writer.WriteLineAsync($"OK {game.GID}");
            }
        }
        catch (Exception e) when (e is IOException or OperationCanceledException or SocketException)
        {
        }
        catch (Exception e)
        {
            logger.LogError(e, "External game registration failed");
        }
        finally
        {
            if (game is not null)
            {
                await connections.RemoveGameListing(game);
                logger.LogInformation("External game {gid} closed", game.GID);
            }
            client.Dispose();
        }
    }
}
