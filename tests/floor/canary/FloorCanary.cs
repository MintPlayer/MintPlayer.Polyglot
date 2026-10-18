// The floor gate's canary (tests/floor/run-floor.ps1). It is compiled INSIDE the net8.0 floor library, beside the
// corpus, and the leg asserts it fails with EXACTLY these two diagnostics — proving that the build ran csc over
// the corpus, that LangVersion 11 is live, and that TreatWarningsAsErrors is live:
//   CS9058  a collection expression is C# 12 (an error at LangVersion 11)
//   CS0168  a declared-but-unused local (a warning, so an error only under TreatWarningsAsErrors)
// Both sit in method bodies on purpose: csc stops reporting method-body warnings once a DECLARATION-level error
// exists (a C# 12 primary constructor here hid the CS0168).
namespace FloorCanary
{
    internal static class Canary
    {
        public static int[] Make()
        {
            int unused;
            return [1, 2];
        }
    }
}
