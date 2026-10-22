namespace Multiroom.Server.Abstractions;

// The server's error model, settled once and used by every module -- the core's
// core/Error.h, carried over.
//
// Each module declares its own small error enum next to the code that can
// produce it and returns Result<T, ThatEnum>. One module is one enum, extended and never reused for another module's failures.
//
// Exceptions never cross a module boundary. Where the framework or a library
// throws, the module that called it catches at its own edge and converts.
//
// The detail string is not decoration: an enum on its own cannot name the key,
// the path or the value that was wrong.
public readonly record struct Failure<TCode>(TCode Code, string Detail)
    where TCode : struct, Enum
{
    // "UnknownKey: logging.levle" -- one log line, one status field.
    public override string ToString() =>
        Detail.Length == 0 ? Code.ToString() : $"{Code}: {Detail}";
}

public readonly struct Result<T, TCode>
    where TCode : struct, Enum
{
    private readonly T? _value;
    private readonly Failure<TCode> _failure;

    private Result(T value)
    {
        _value = value;
        _failure = default;
        IsSuccess = true;
    }

    private Result(Failure<TCode> failure)
    {
        _value = default;
        _failure = failure;
        IsSuccess = false;
    }

    public bool IsSuccess { get; }

    // Reading the wrong half is a bug in the caller, not a runtime condition,
    // so it throws rather than answering a default.
    public T Value => IsSuccess
        ? _value!
        : throw new InvalidOperationException($"Result holds a failure: {_failure}");

    public Failure<TCode> Failure => !IsSuccess
        ? _failure
        : throw new InvalidOperationException("Result holds a value, not a failure");

    public static implicit operator Result<T, TCode>(T value) => new(value);
    public static implicit operator Result<T, TCode>(Failure<TCode> failure) => new(failure);

    public override string ToString() => IsSuccess ? $"Ok({_value})" : _failure.ToString();
}

public static class Result
{
    public static Failure<TCode> Fail<TCode>(TCode code, string detail = "")
        where TCode : struct, Enum => new(code, detail);
}
