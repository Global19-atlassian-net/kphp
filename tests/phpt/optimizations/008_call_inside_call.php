@ok
<?php

function f($x) {
    global $a;
    $a += 1;
    return $x;
}

$a = 5;

$res = f(f($a));
echo $res . "\n";
