using System;

static class Program
{
    static System.Collections.Generic.IEnumerable<int> countUp(int n)
    {
        var i = 0;
        while (i < n)
        {
            yield return i;
            i = i + 1;
        }
    }
    static void main()
    {
        var total = 0;
        foreach (var v in countUp(5))
        {
            total = total + v;
        }
        Console.WriteLine(total);
        foreach (var v in countUp(3))
        {
            Console.WriteLine(v);
        }
    }
    
    static void Main() { main(); }
}
