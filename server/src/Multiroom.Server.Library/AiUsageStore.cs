using System.Globalization;
using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// One day's AI usage by one origin on one provider's model.
public sealed record AiUsageDay(string Day, string Origin, string Provider, string Model, long Calls, long Failures,
    long InputTokens, long OutputTokens, long ReasoningTokens, long WebSearches);

// The AI's token usage per day, in the library's database (Migrations/0022):
// each call sent to a vendor is added to its day's row. Kept without limit.
public sealed class AiUsageStore(LibraryDatabase database)
{
    private const string DayFormat = "yyyy-MM-dd";

    public Result<bool, LibraryError> Record(DateOnly day, string origin, string provider, string model, bool failed,
        long inputTokens, long outputTokens, long reasoningTokens, long webSearches) =>
        Run<bool>(c =>
        {
            using var command = Command(c, """
                INSERT INTO ai_usage (day, origin, provider, model, calls, failures, input_tokens, output_tokens,
                    reasoning_tokens, web_searches)
                VALUES ($day, $origin, $provider, $model, 1, $failed, $input, $output, $reasoning, $searches)
                ON CONFLICT (day, origin, provider, model) DO UPDATE SET
                    calls = calls + 1, failures = failures + excluded.failures,
                    input_tokens = input_tokens + excluded.input_tokens,
                    output_tokens = output_tokens + excluded.output_tokens,
                    reasoning_tokens = reasoning_tokens + excluded.reasoning_tokens,
                    web_searches = web_searches + excluded.web_searches
                """, ("$day", day.ToString(DayFormat, CultureInfo.InvariantCulture)), ("$origin", origin),
                ("$provider", provider), ("$model", model), ("$failed", failed ? 1 : 0), ("$input", inputTokens),
                ("$output", outputTokens), ("$reasoning", reasoningTokens), ("$searches", webSearches));
            command.ExecuteNonQuery();
            return true;
        });

    // Every row from `from` to `to`, both included (each open when null),
    // oldest day first.
    public Result<IReadOnlyList<AiUsageDay>, LibraryError> Days(DateOnly? from, DateOnly? to) =>
        Run<IReadOnlyList<AiUsageDay>>(c =>
        {
            using var command = Command(c, """
                SELECT day, origin, provider, model, calls, failures, input_tokens, output_tokens, reasoning_tokens,
                    web_searches
                FROM ai_usage WHERE ($from IS NULL OR day >= $from) AND ($to IS NULL OR day <= $to)
                ORDER BY day, origin, provider, model
                """, ("$from", from?.ToString(DayFormat, CultureInfo.InvariantCulture)),
                ("$to", to?.ToString(DayFormat, CultureInfo.InvariantCulture)));
            using var reader = command.ExecuteReader();
            var rows = new List<AiUsageDay>();
            while (reader.Read())
            {
                rows.Add(new AiUsageDay(reader.GetString(0), reader.GetString(1), reader.GetString(2),
                    reader.GetString(3), reader.GetInt64(4), reader.GetInt64(5), reader.GetInt64(6),
                    reader.GetInt64(7), reader.GetInt64(8), reader.GetInt64(9)));
            }
            return rows;
        });

    private static SqliteCommand Command(SqliteConnection c, string sql, params (string Name, object? Value)[] parameters)
    {
        var command = c.CreateCommand();
#pragma warning disable CA2100 // Only this class's own statements; every value is a parameter.
        command.CommandText = sql;
#pragma warning restore CA2100
        foreach (var (name, value) in parameters)
            command.Parameters.AddWithValue(name, value ?? DBNull.Value);
        return command;
    }

    private Result<T, LibraryError> Run<T>(Func<SqliteConnection, Result<T, LibraryError>> run)
    {
        try
        {
            using var connection = database.Open();
            return run(connection);
        }
        catch (SqliteException e)
        {
            return Result.Fail(LibraryError.UnusableDatabase, e.Message);
        }
    }
}
