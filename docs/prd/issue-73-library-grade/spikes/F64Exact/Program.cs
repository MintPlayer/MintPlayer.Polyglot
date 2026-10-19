using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;

static class P
{
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    // Layout: given a .NET numeric string (plain or with E), redo the ECMAScript Number::toString layout.
    static string Layout(string s)
    {
        bool neg = false; int i = 0;
        if (s[0] == '-') { neg = true; i = 1; }
        int ePos = s.IndexOfAny(new[] { 'E', 'e' });
        string mant = ePos < 0 ? s.Substring(i) : s.Substring(i, ePos - i);
        int exp = ePos < 0 ? 0 : int.Parse(s.Substring(ePos + 1), NumberStyles.AllowLeadingSign, Inv);
        int dot = mant.IndexOf('.');
        string digits = dot < 0 ? mant : mant.Substring(0, dot) + mant.Substring(dot + 1);
        int intLen = dot < 0 ? mant.Length : dot;
        // strip leading zeros
        int lz = 0; while (lz < digits.Length - 1 && digits[lz] == '0') lz++;
        digits = digits.Substring(lz); intLen -= lz;
        digits = digits.TrimEnd('0');
        if (digits.Length == 0) return "0";
        int k = digits.Length;
        int n = intLen + exp; // value = 0.digits * 10^n
        var sb = new StringBuilder(32);
        if (neg) sb.Append('-');
        if (k <= n && n <= 21) { sb.Append(digits); sb.Append('0', n - k); }
        else if (0 < n && n <= 21) { sb.Append(digits, 0, n); sb.Append('.'); sb.Append(digits, n, k - n); }
        else if (-6 < n && n <= 0) { sb.Append("0."); sb.Append('0', -n); sb.Append(digits); }
        else
        {
            int e = n - 1;
            sb.Append(digits[0]);
            if (k > 1) { sb.Append('.'); sb.Append(digits, 1, k - 1); }
            sb.Append('e'); sb.Append(e >= 0 ? '+' : '-'); sb.Append(Math.Abs(e).ToString(Inv));
        }
        return sb.ToString();
    }

    static string Special(double x)
    {
        if (double.IsNaN(x)) return "NaN";
        if (double.IsPositiveInfinity(x)) return "Infinity";
        if (double.IsNegativeInfinity(x)) return "-Infinity";
        if (x == 0) return "0";
        return null;
    }

    public static string StrR(double x) => Special(x) ?? Layout(x.ToString("R", Inv));

    static bool RT(string s, double x) => double.TryParse(s, NumberStyles.Float, Inv, out double d) && d == x;
    // Classic portable formulation: shortest of 15/16/17 significant digits that round-trips.
    public static string StrSearch(double x)
    {
        string sp = Special(x); if (sp != null) return sp;
        string s = x.ToString("E14", Inv);
        if (!RT(s, x))
        {
            s = x.ToString("E15", Inv);
            if (!RT(s, x)) s = x.ToString("E16", Inv);
        }
        return Layout(s);
    }

    // Search 1..17 for subnormals (precision < 15 digits there), 15..17 otherwise.
    public static string StrSearchFull(double x)
    {
        string sp = Special(x); if (sp != null) return sp;
        int start = Math.Abs(x) < 2.2250738585072014e-308 ? 1 : 15;
        for (int p = start; p < 17; p++) { string s = x.ToString("E" + (p - 1).ToString(Inv), Inv); if (RT(s, x)) return Layout(s); }
        return Layout(x.ToString("E16", Inv));
    }

    // Hybrid: 15-digit fast path (normal, non-power-of-two), exact B&D otherwise.
    public static string StrHybrid(double x)
    {
        string sp = Special(x); if (sp != null) return sp;
        long bits = BitConverter.DoubleToInt64Bits(x);
        if (((bits >> 52) & 0x7FF) != 0 && (bits & 0xFFFFFFFFFFFFFL) != 0)
        {
            string s = x.ToString("E14", Inv);
            if (RT(s, x)) return Layout(s);
        }
        return StrExact(x);
    }

    public static string StrG17(double x) => Special(x) ?? Layout(x.ToString("G17", Inv));

    // ---- Runtime-independent exact shortest (Burger & Dybvig free-format, BigInteger) ----
    static readonly System.Numerics.BigInteger[] P10 = MakeP10();
    static System.Numerics.BigInteger[] MakeP10() { var a = new System.Numerics.BigInteger[400]; a[0] = 1; for (int i = 1; i < 400; i++) a[i] = a[i - 1] * 10; return a; }
    public static string StrExact(double x)
    {
        string sp = Special(x); if (sp != null) return sp;
        bool neg = x < 0; if (neg) x = -x;
        long bits = BitConverter.DoubleToInt64Bits(x);
        int be = (int)((bits >> 52) & 0x7FF); long f = bits & 0xFFFFFFFFFFFFFL; int e;
        if (be == 0) e = -1074; else { f |= 1L << 52; e = be - 1075; }
        bool even = (f & 1) == 0;
        bool asym = f == (1L << 52) && be > 1;
        System.Numerics.BigInteger r, s, mp, mm;
        if (e >= 0)
        {
            var b = System.Numerics.BigInteger.One << e;
            if (!asym) { r = f * b * 2; s = 2; mp = b; mm = b; } else { r = f * b * 4; s = 4; mp = b * 2; mm = b; }
        }
        else
        {
            if (!asym) { r = (System.Numerics.BigInteger)f * 2; s = System.Numerics.BigInteger.One << (1 - e); mp = 1; mm = 1; }
            else { r = (System.Numerics.BigInteger)f * 4; s = System.Numerics.BigInteger.One << (2 - e); mp = 2; mm = 1; }
        }
        int k = (int)Math.Ceiling(Math.Log10(x) - 1e-10);
        if (k >= 0) s *= P10[k]; else { var p = P10[-k]; r *= p; mp *= p; mm *= p; }
        // fix up the estimate so that 10^(k-1) <= high < 10^k
        if (even ? r + mp >= s : r + mp > s) { s *= 10; k++; }
        else
        {
            while (even ? (r + mp) * 10 < s : (r + mp) * 10 <= s) { r *= 10; mp *= 10; mm *= 10; k--; }
        }
        var dg = new StringBuilder(20);
        while (true)
        {
            r *= 10; mp *= 10; mm *= 10;
            int d = (int)System.Numerics.BigInteger.DivRem(r, s, out r);
            bool tc1 = even ? r <= mm : r < mm;
            bool tc2 = even ? r + mp >= s : r + mp > s;
            if (!tc1 && !tc2) { dg.Append((char)('0' + d)); continue; }
            if (tc1 && tc2) { var c = (r * 2).CompareTo(s); if (c > 0 || (c == 0 && (d & 1) == 1)) d++; }
            else if (tc2) d++;
            dg.Append((char)('0' + d));
            break;
        }
        return Lay(neg, dg.ToString(), k);
    }
    static string Lay(bool neg, string digits, int n)
    {
        int k = digits.Length; var sb = new StringBuilder(32); if (neg) sb.Append('-');
        if (k <= n && n <= 21) { sb.Append(digits); sb.Append('0', n - k); }
        else if (0 < n && n <= 21) { sb.Append(digits, 0, n); sb.Append('.'); sb.Append(digits, n, k - n); }
        else if (-6 < n && n <= 0) { sb.Append("0."); sb.Append('0', -n); sb.Append(digits); }
        else { int e = n - 1; sb.Append(digits[0]); if (k > 1) { sb.Append('.'); sb.Append(digits, 1, k - 1); } sb.Append('e'); sb.Append(e >= 0 ? '+' : '-'); sb.Append(Math.Abs(e).ToString(Inv)); }
        return sb.ToString();
    }

    static int Main(string[] args)
    {
        string[] lines = File.ReadAllLines(args[0]);
        int N = lines.Length;
        var xs = new double[N]; var exp = new string[N];
        for (int i = 0; i < N; i++)
        {
            int sp = lines[i].IndexOf(' ');
            xs[i] = BitConverter.Int64BitsToDouble(unchecked((long)ulong.Parse(lines[i].Substring(0, sp), NumberStyles.HexNumber, Inv)));
            exp[i] = lines[i].Substring(sp + 1);
        }
        Console.WriteLine("runtime: " + System.Runtime.InteropServices.RuntimeInformation.FrameworkDescription + " x64=" + Environment.Is64BitProcess + " N=" + N);
        Run("R+layout", StrR, xs, exp);
        Run("E14/E15/E16 search", StrSearch, xs, exp);
        Run("search 1..17 subnormal", StrSearchFull, xs, exp);
        Run("G17+layout (not shortest)", StrG17, xs, exp);
        Run("exact BigInteger B&D", StrExact, xs, exp);
        Run("hybrid E14-RT + exact", StrHybrid, xs, exp);
        double t = Math.Pow(2, -25);
        Console.WriteLine("2^-25: R=" + t.ToString("R", Inv) + " default=" + t.ToString(Inv) + " G17=" + t.ToString("G17", Inv));
        // R round-trip faithfulness itself
        int rtFail = 0;
        for (int i = 0; i < N; i++) { double x = xs[i]; if (double.IsNaN(x)) continue; if (BitConverter.DoubleToInt64Bits(double.Parse(x.ToString("R", Inv), Inv)) != BitConverter.DoubleToInt64Bits(x) && x != 0) rtFail++; }
        Console.WriteLine("R parse round-trip failures: " + rtFail);
        return 0;
    }

    static void Run(string name, Func<double, string> f, double[] xs, string[] exp)
    {
        for (int i = 0; i < 2000; i++) f(xs[i % xs.Length]); // warm
        int bad = 0, shown = 0; var sw = Stopwatch.StartNew();
        var got = new string[xs.Length];
        for (int i = 0; i < xs.Length; i++) got[i] = f(xs[i]);
        sw.Stop();
        var cats = new StringBuilder();
        for (int i = 0; i < xs.Length; i++)
            if (got[i] != exp[i]) { bad++; if (shown++ < 6) cats.Append("    want " + exp[i] + "  got " + got[i] + "\n"); }
        Console.WriteLine(string.Format(Inv, "{0,-28} mismatches={1,7}  {2,7:F0} ms  ({3:F0} ns/op)", name, bad, sw.Elapsed.TotalMilliseconds, sw.Elapsed.TotalMilliseconds * 1e6 / xs.Length));
        Console.Write(cats.ToString());
    }
}
