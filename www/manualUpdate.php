<?
header("Access-Control-Allow-Origin: *");

$wrapped = 0;

if (isset($_GET['wrapped']))
  $wrapped = 1;

if (!$wrapped)
  echo "<html>\n";

$skipJSsettings = 1;
require_once("common.php");

DisableOutputBuffering();

// Emit a "===== <message> =====" stage header, matching the shell helper
// logStage() in scripts/common, so the streaming FPP Upgrade dialog can drive
// its status line from PHP-side phases too (git_pull emits its own). Reads as a
// section header in the log and doubles as the machine-parsed progress marker.
function logStage($msg)
{
  echo "===== $msg =====\n";
}

if (!$wrapped) {
  ?>

  <head>
    <title>
      FPP Manual Update
    </title>
  </head>

  <body>
    <h2>FPP Manual Update</h2>
    <pre>
        <?
}
?>
Pulling in updates...
<?
$startTime = microtime(true);
system("$fppDir/scripts/git_pull", $return_val);
$endTime = microtime(true);
$diffTime = round($endTime - $startTime);

$h = floor($diffTime / 3600);
$m = floor($diffTime % 3600 / 60);
$s = floor($diffTime % 60);

printf("----------------------\nElapsed Time: %02d:%02d:%02d\n", $h, $m, $s);
?>
<?

if ($return_val === 0) {
  logStage("Restarting FPP");

  // Compare and copy apache config if needed
  $srcConf = "/opt/fpp/etc/apache2.site";
  $dstConf = "/etc/apache2/sites-enabled/000-default.conf";
  $needCopy = true;

  if (file_exists($srcConf) && file_exists($dstConf)) {
    // Compare file hashes
    if (md5_file($srcConf) === md5_file($dstConf)) {
      $needCopy = false;
    }
  }

  if ($needCopy) {
    echo "Updating Apache config...\n";
    // Use sudo if needed for permissions
    system("$SUDO cp $srcConf $dstConf");
  } else {
    echo "Apache config is already up to date.\n";
  }

  touch("$mediaDirectory/tmp/fppd_restarted");

  system($SUDO . " $fppDir/scripts/fppd_restart");

  system($SUDO . " $fppDir/scripts/ManageApacheContentPolicy regenerate");

  if (file_exists($settings['statsFile'])) {
    unlink($settings['statsFile']);
  }

  exec($SUDO . " rm -f /tmp/cache_*.cache");
  if (file_exists($fppDir . "/src/fppd")) {
    logStage("Upgrade Complete");
  } else {
    logStage("Upgrade Failed");
    print ("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    print ("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    print ("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    print ("Upgrade FAILED.\n");
    print ("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    print ("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    print ("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
  }
} else {
  logStage("Upgrade Failed");
  print ("Upgrade FAILED. See above for details.\n");
}

if (!$wrapped) {
  ?>
        <a href='index.php'>Go to FPP Main Status Page</a><br>
        <a href='about.php'>Go back to FPP Upgrade page</a><br>

        </body>
        </html>
        <?
}
?>
