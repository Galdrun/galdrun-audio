using System.Text.Json;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Configuration;

namespace Multiroom.Server.Tests;

public sealed class ApiErrorTests
{
    [Fact]
    public void A_failure_maps_to_the_outward_error_shape()
    {
        Result<int, ConfigError> result = Result.Fail(ConfigError.UnknownKey, "logging.levle");

        var error = ApiError.From(result.Failure);

        Assert.Equal("""{"error":"unknownKey","message":"logging.levle"}""", JsonSerializer.Serialize(error));
        Assert.Equal("UnknownKey: logging.levle", result.Failure.ToString());
    }
}
