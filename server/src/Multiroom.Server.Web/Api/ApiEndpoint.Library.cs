using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Library.Smb;

namespace Multiroom.Server.Web.Api;

// The library's folders (phase 13): listed from both lists, and those of the
// API added, changed and removed; a pass queued for any of them; and a NAS's
// shares and directories listed, so a UI picks a path instead of typing it.
// A password goes in a body, never a URL, and is never answered: a folder
// says hasPassword.
public sealed partial class ApiEndpoint
{
    private void MapLibrary(WebApplication app)
    {
        app.MapGet("/api/v1/library/folders", () => Json(indexer.Folders));
        app.MapPost("/api/v1/library/folders", (Delegate)((HttpContext c) =>
            WithBody<LibraryFolderCreate, LibraryFolderView, LibraryError>(c,
                r => Task.FromResult(AddFolder(r)), StatusCodes.Status201Created)));
        app.MapMethods("/api/v1/library/folders/{id}", [HttpMethods.Patch], (string id, HttpContext c) =>
            WithBody<LibraryFolderPatch, LibraryFolderView, LibraryError>(c, r => Task.FromResult(PatchFolder(id, r))));
        app.MapDelete("/api/v1/library/folders/{id}", (string id) => Deleted(folders.Remove(id)));
        app.MapPost("/api/v1/library/folders/{id}/scan", (string id, HttpContext c) =>
            c.Request.ContentLength > 0
                ? Error(Result.Fail(ApiRequestError.InvalidRequest, $"{c.Request.Path} takes no body"))
                : Answer(indexer.Scan(id), StatusCodes.Status202Accepted));

        app.MapPost("/api/v1/library/smb/shares", (Delegate)((HttpContext c) =>
            WithBody<SmbSharesRequest, IReadOnlyList<string>, LibraryError>(c, r => ListShares(r, c.RequestAborted))));
        app.MapPost("/api/v1/library/smb/directories", (Delegate)((HttpContext c) =>
            WithBody<SmbDirectoriesRequest, IReadOnlyList<string>, LibraryError>(c, r => ListDirectories(r, c.RequestAborted))));
    }

    private Result<LibraryFolderView, LibraryError> AddFolder(LibraryFolderCreate request)
    {
        SmbLocation? smb = null;
        if (request.Type == LibraryFolderType.Smb)
        {
            if (request.Host is null || request.Share is null)
                return Result.Fail(LibraryError.InvalidArgument, "an smb folder needs host and share");
            var location = Location(request.Host, request.Port, request.Share, request.Path, request.User,
                request.Password, request.Domain);
            if (!location.IsSuccess)
                return location.Failure;
            smb = location.Value;
        }
        else if (request.Host is not null || request.Port is not null || request.Share is not null ||
                 request.User is not null || request.Password is not null || request.Domain is not null)
        {
            return Result.Fail(LibraryError.InvalidArgument, "host, port, share and the login are an smb folder's");
        }

        var added = folders.Add(request.Type, request.Path, smb, request.Enabled, request.MediaType);
        return added.IsSuccess ? indexer.Folder(added.Value.Id)! : added.Failure;
    }

    private Result<LibraryFolderView, LibraryError> PatchFolder(string id, LibraryFolderPatch patch)
    {
        if (patch is { Enabled: null, User: null, Password: null, Domain: null, MediaType: null })
        {
            return Result.Fail(LibraryError.InvalidArgument,
                "a folder patch needs enabled, user, password, domain or mediaType");
        }
        (MediaType?, bool) mediaType = default;
        if (patch.MediaType is { } named)
        {
            if (named.Length == 0)
                mediaType = (null, true);
            else if (Enum.TryParse<MediaType>(named, ignoreCase: true, out var parsed) && !int.TryParse(named, out _))
                mediaType = (parsed, true);
            else
                return Result.Fail(LibraryError.InvalidArgument, $"mediaType: music, audiobook or \"\", was {named}");
        }
        var updated = folders.Update(id, patch.Enabled, patch.User, patch.Password, patch.Domain, mediaType);
        return updated.IsSuccess ? indexer.Folder(id)! : updated.Failure;
    }

    private Task<Result<IReadOnlyList<string>, LibraryError>> ListShares(SmbSharesRequest request,
        CancellationToken cancellationToken)
    {
        var login = Login(request.Host, request.Port, request.User, request.Password, request.Domain);
        return login.IsSuccess
            ? OnShare(login.Value, lease => lease.ListShares(), cancellationToken)
            : Task.FromResult<Result<IReadOnlyList<string>, LibraryError>>(login.Failure);
    }

    // The directories inside a share, or inside a directory of it.
    private Task<Result<IReadOnlyList<string>, LibraryError>> ListDirectories(SmbDirectoriesRequest request,
        CancellationToken cancellationToken)
    {
        var location = Location(request.Host, request.Port, request.Share, request.Path, request.User, request.Password,
            request.Domain);
        if (!location.IsSuccess)
            return Task.FromResult<Result<IReadOnlyList<string>, LibraryError>>(location.Failure);
        var share = location.Value;
        return OnShare(share.Login, lease =>
        {
            var listed = lease.List(share.Share, share.Directory.Replace('/', '\\'));
            if (!listed.IsSuccess)
                return listed.Failure;
            return listed.Value.Where(e => e.IsDirectory).Select(e => e.Name)
                .Order(StringComparer.OrdinalIgnoreCase).ToList();
        }, cancellationToken);
    }

    // SMB is synchronous: it runs off the request's thread, on a session of
    // the pool, which bounds how long it may take to connect.
    private Task<Result<IReadOnlyList<string>, LibraryError>> OnShare(SmbLogin login,
        Func<SmbLease, Result<IReadOnlyList<string>, LibraryError>> request, CancellationToken cancellationToken) =>
        Task.Run(() =>
        {
            var leased = folders.Sessions.Lease(login);
            if (!leased.IsSuccess)
                return leased.Failure;
            using var lease = leased.Value;
            return request(lease);
        }, cancellationToken);

    private static Result<SmbLogin, LibraryError> Login(string host, int? port, string? user, string? password,
        string? domain)
    {
        if (string.IsNullOrWhiteSpace(host) || host.IndexOfAny([':', '/', '\\']) >= 0)
            return Result.Fail(LibraryError.InvalidArgument, $"host: a name or an address, was {host}");
        if (port is < 1 or > 65535)
            return Result.Fail(LibraryError.InvalidArgument, $"port: 1 to 65535, was {port}");
        return new SmbLogin(host, port ?? SmbLogin.DefaultPort, user, password, domain);
    }

    private static Result<SmbLocation, LibraryError> Location(string host, int? port, string share, string? path,
        string? user, string? password, string? domain)
    {
        var login = Login(host, port, user, password, domain);
        if (!login.IsSuccess)
            return login.Failure;
        if (string.IsNullOrWhiteSpace(share) || share.IndexOfAny(['/', '\\']) >= 0)
            return Result.Fail(LibraryError.InvalidArgument, $"share: a share's name, was {share}");
        try
        {
            return new SmbLocation(login.Value, share, path);
        }
        catch (ArgumentException)
        {
            return Result.Fail(LibraryError.InvalidArgument, $"path: no . or .. segments, was {path}");
        }
    }
}
