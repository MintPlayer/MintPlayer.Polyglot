// PgCultureHook — a .NET startup hook (DOTNET_STARTUP_HOOKS=<path>/PgCultureHook.dll) for the P40 culture
// conformance leg. It forces the process culture to $PG_CULTURE BEFORE the program's Main runs, then asserts the
// culture really took effect, so a host with no ICU (or DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1, or
// PredefinedCulturesOnly fallbacks) fails loudly instead of passing vacuously with invariant data.
//
// Contract:
//   PG_CULTURE unset/empty  -> no-op.
//   culture applied + probe ok -> silent (stdout stays byte-identical to the program's own output).
//   anything else              -> one "PgCultureHook: ..." line on stderr, exit code 97.
//
// The type must be named StartupHook, live in no namespace, and expose `static void Initialize()`.
using System;
using System.Globalization;

internal static class StartupHook
{
    public const int FailExit = 97;

    public static void Initialize()
    {
        string? name = Environment.GetEnvironmentVariable("PG_CULTURE");
        if (string.IsNullOrEmpty(name)) return;

        CultureInfo ci;
        try
        {
            // GetCultureInfo throws CultureNotFoundException in invariant-globalization mode (PredefinedCulturesOnly
            // defaults to true there), and on ICU hosts that lack the locale.
            ci = CultureInfo.GetCultureInfo(name);
        }
        catch (Exception ex)
        {
            Fail($"culture '{name}' unavailable ({ex.GetType().Name}: {ex.Message})");
            return;
        }

        CultureInfo.DefaultThreadCurrentCulture = ci;
        CultureInfo.DefaultThreadCurrentUICulture = ci;
        CultureInfo.CurrentCulture = ci;      // the hook runs on the thread that then runs Main
        CultureInfo.CurrentUICulture = ci;

        string? why = Probe(name);
        if (why != null) Fail($"culture '{name}' did not take effect: {why}");
    }

    // Returns null when the current culture behaves like real data for `name`, else the reason it does not.
    internal static string? Probe(string name)
    {
        CultureInfo cur = CultureInfo.CurrentCulture;
        if (!string.Equals(cur.Name, name, StringComparison.OrdinalIgnoreCase))
            return $"CurrentCulture is '{cur.Name}'";
        NumberFormatInfo nf = cur.NumberFormat;
        switch (name.ToLowerInvariant())
        {
            case "fa-ir":
                // ICU fa-IR: U+200E LRM + U+2212 MINUS SIGN (or at least anything that isn't ASCII '-')
                if (nf.NegativeSign == "-") return "fa-IR NegativeSign is ASCII '-'";
                break;
            case "tr-tr":
                if ("I".ToLower() != "ı") return "tr-TR \"I\".ToLower() is not dotless i (U+0131)";
                break;
            case "sv-se":
                if (nf.NegativeSign != "−") return $"sv-SE NegativeSign is {Hex(nf.NegativeSign)}, want U+2212";
                if (nf.NumberDecimalSeparator != ",") return "sv-SE decimal separator is not ','";
                break;
            case "nl-be":
                if (nf.NumberDecimalSeparator != ",") return "nl-BE decimal separator is not ','";
                break;
            default:
                // Generic hostile/synthetic culture: require it to differ from invariant somewhere observable.
                if (nf.NumberDecimalSeparator == "." && nf.NegativeSign == "-" && "I".ToLower() == "i")
                    return "culture is indistinguishable from invariant (decimal '.', sign '-', I->i)";
                break;
        }
        return null;
    }

    static string Hex(string s)
    {
        var parts = new string[s.Length];
        for (int i = 0; i < s.Length; i++) parts[i] = "U+" + ((int)s[i]).ToString("X4", CultureInfo.InvariantCulture);
        return string.Join(" ", parts);
    }

    static void Fail(string msg)
    {
        Console.Error.WriteLine("PgCultureHook: " + msg);
        Environment.Exit(FailExit);
    }
}
