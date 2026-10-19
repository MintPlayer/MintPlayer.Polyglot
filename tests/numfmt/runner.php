<?php
// P40 A3 corpus runner (PHP). Evaluates the php plugin's SHIPPED `pgStr` prelude (`__pg_str`) from the
// manifest and checks it against the ECMAScript reference text. Args: <manifest> <corpus>. The gate runs it
// with a hostile `-d precision=17 -d serialize_precision=17`: the helper must not depend on either setting.
$manifest = json_decode(file_get_contents($argv[1]), true);
eval($manifest['spec']['preludes']['pgStr']);
$before = ini_get('serialize_precision');
$n = 0;
$bad = 0;
foreach (file($argv[2], FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $line) {
    $sp = strpos($line, ' ');
    $x = unpack('E', hex2bin(substr($line, 0, $sp)))[1];
    $want = substr($line, $sp + 1);
    $got = __pg_str($x);
    $n++;
    if ($got !== $want) {
        $bad++;
        if ($bad <= 10) {
            echo "  PHP mismatch " . substr($line, 0, $sp) . ": want $want got $got\n";
        }
    }
}
if (ini_get('serialize_precision') !== $before) {
    echo "  PHP: __pg_str did not restore serialize_precision\n";
    $bad++;
}
echo "PHP    : $n doubles, $bad mismatches\n";
exit($bad === 0 ? 0 : 1);
