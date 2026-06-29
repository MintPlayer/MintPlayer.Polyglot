function* countUp(n: number): Iterable<number> {
    let i = 0;
    while (i < n) {
        yield i;
        i = i + 1;
    }
}
function main(): void {
    let total = 0;
    for (const v of countUp(5)) {
        total = total + v;
    }
    console.log(total);
    for (const v of countUp(3)) {
        console.log(v);
    }
}
main();
