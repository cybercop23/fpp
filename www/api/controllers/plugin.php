<?

// Shared system-package helpers (hardened apt + per-requester ownership) used
// when installing/removing a plugin's declared package dependencies.
require_once __DIR__ . '/../../common/packages.inc.php';

// True if a dependencies block declares at least one non-empty apt package.
function DepsRequirePackages($deps)
{
	if (!is_array($deps) || !isset($deps['packages']) || !is_array($deps['packages'])) {
		return false;
	}
	foreach ($deps['packages'] as $p) {
		if (is_string($p) && $p !== '') {
			return true;
		}
	}
	return false;
}

// Removes a partially-installed plugin directory (and any linkName symlink) so a
// refused/failed install does not leave a half-installed plugin behind.
function CleanupPartialPluginInstall($plugin, $linkName = null)
{
	global $settings, $SUDO;
	if (is_string($linkName) && $linkName !== '') {
		exec($SUDO . " rm -f " . escapeshellarg($settings['pluginDirectory'] . '/' . $linkName));
	}
	exec($SUDO . " rm -rf " . escapeshellarg($settings['pluginDirectory'] . '/' . $plugin));
}

/**
 * Get all plugins
 *
 * Get list of installed plugins.
 *
 * @route GET /api/plugin
 * @response 200 List of installed plugin names
 * ```json
 * ["fpp-brightness", "fpp-matrixtools", "fpp-vastfmt"]
 * ```
 */
function GetInstalledPlugins()
{
	global $settings;
	$plugins = array();

	$dir = $settings['pluginDirectory'];

	if ($dh = opendir($dir)) {
		while (($file = readdir($dh)) !== false) {
			if (
				(!in_array($file, array('.', '..'))) &&
				(is_dir($dir . '/' . $file)) &&
				(file_exists($dir . '/' . $file . '/pluginInfo.json'))
			) {
				array_push($plugins, $file);
			}
		}
	}

	return json($plugins);
}

/**
 * Install plugin
 *
 * Install a new plugin. The request body is a `pluginInfo.json` structure
 * with `branch` and `sha` fields added to specify which branch and commit
 * to install.
 *
 * @route POST /api/plugin
 * @body {"repoName": "fpp-matrixtools", "name": "MatrixTools", "author": "Chris Pinkham (CaptainMurdoch)", "srcURL": "https://github.com/cpinkham/fpp-matrixtools.git", "branch": "master", "sha": ""}
 * @response 200 Plugin installed
 * ```json
 * {"Status": "OK", "Message": ""}
 * ```
 */
function InstallPlugin()
{
	global $settings, $_REQUEST;
	$result = array();

	$pluginInfoJSON = "";
	$postdata = fopen("php://input", "r");
	while ($data = fread($postdata, 1024 * 16)) {
		$pluginInfoJSON .= $data;
	}
	fclose($postdata);

	$pluginInfo = json_decode($pluginInfoJSON, true);
	if (!is_array($pluginInfo) || !isset($pluginInfo['repoName'])) {
		$result['Status'] = 'Error';
		$result['Message'] = 'Invalid pluginInfo (missing repoName)';
		return json($result);
	}

	$stream = isset($_REQUEST['stream']) ? $_REQUEST['stream'] : null;
	$streaming = (isset($stream) && $stream != "false");
	$plugin = escapeshellcmd($pluginInfo['repoName']);

	if (file_exists($settings['pluginDirectory'] . '/' . $plugin)) {
		if ($streaming) {
			DisableOutputBuffering();
			echo "The (" . $plugin . ") plugin is already installed\n";
			return "\nDone\n";
		}
		$result['Status'] = 'Error';
		$result['Message'] = 'The (' . $plugin . ') plugin is already installed';
		return json($result);
	}

	if ($streaming) {
		DisableOutputBuffering();
	}

	// $visited guards against dependency cycles (A depends on B depends on A)
	// across the recursive install below.
	$visited = array();
	$ok = InstallPluginFromInfo($pluginInfo, $visited, $stream, 0);

	if ($streaming) {
		return "\nDone\n";
	}
	$result['Status'] = $ok ? 'OK' : 'Error';
	$result['Message'] = $ok ? '' : 'Could not properly install plugin';
	return json($result);
}

/**
 * Installs a single plugin from a decoded pluginInfo structure, resolving its
 * declared dependencies (packages, scripts, plugins) BEFORE running the
 * plugin's own fpp_install.sh. Recurses into dependency plugins with a shared
 * $visited set (cycle guard) and a depth cap. Output is streamed to the client
 * when $stream is truthy. Returns true on success.
 */
function InstallPluginFromInfo($pluginInfo, &$visited, $stream, $depth = 0)
{
	global $settings, $fppDir, $SUDO;

	$streaming = (isset($stream) && $stream != "false");

	if (!is_array($pluginInfo) || !isset($pluginInfo['repoName'])) {
		echo "\nERROR: dependency plugin info missing repoName\n";
		return false;
	}
	$repoName = $pluginInfo['repoName'];
	$plugin = escapeshellcmd($repoName);

	// Cycle guard.
	if (isset($visited[$repoName])) {
		return true;
	}
	$visited[$repoName] = true;

	if ($depth > 8) {
		echo "\nERROR: plugin dependency chain too deep at '$repoName'; aborting.\n";
		return false;
	}

	// Already installed -> dependency is satisfied.
	if (file_exists($settings['pluginDirectory'] . '/' . $plugin)) {
		if ($depth > 0) {
			echo "\nDependency plugin '$plugin' is already installed.\n";
		}
		return true;
	}

	// Refuse up front if the plugin declares required apt packages but this
	// platform has no apt -- installing it would leave it broken. A plugin that
	// genuinely needs packages should restrict itself with platforms[]; if the
	// author forgot, we catch it here rather than half-installing. (Re-checked
	// after clone against the repo's own pluginInfo.json, which is authoritative.)
	if (DepsRequirePackages(isset($pluginInfo['dependencies']) ? $pluginInfo['dependencies'] : null) && !AptAvailable()) {
		$pkgs = implode(', ', $pluginInfo['dependencies']['packages']);
		$plat = isset($settings['Platform']) ? $settings['Platform'] : 'this platform';
		echo "\nERROR: '$repoName' requires system packages ($pkgs) but $plat does not support system packages. Refusing to install.\n";
		return false;
	}

	$srcURL = isset($pluginInfo['srcURL']) ? $pluginInfo['srcURL'] : '';
	$branch = escapeshellcmd(isset($pluginInfo['branch']) && $pluginInfo['branch'] !== '' ? $pluginInfo['branch'] : 'master');
	$sha = isset($pluginInfo['sha']) ? $pluginInfo['sha'] : '';
	$infoURL = isset($pluginInfo['infoURL']) ? $pluginInfo['infoURL'] : '';
	$useCredentials = isset($pluginInfo['useCredentials']) && $pluginInfo['useCredentials'];

	// Inject GitHub credentials for github URLs when configured. Only modifies
	// github.com / raw.githubusercontent.com URLs so creds never leak elsewhere.
	$injectedURL = InjectGitHubCredentials($srcURL);
	if ($injectedURL !== false) {
		$srcURL = $injectedURL;
	} else if ($useCredentials) {
		echo "\nERROR: Use Credentials was selected but GitHub user name and/or Personal Access Token are not configured on the Developer settings page.\n";
		return false;
	}

	// Clone ONLY -- the plugin's own fpp_install.sh is deferred (via
	// FPP_SKIP_INSTALL_SCRIPT) so dependencies can be resolved first.
	$return_val = 0;
	$envPrefix = "export FPP_SKIP_INSTALL_SCRIPT=1; export SUDO=\"" . $SUDO . "\"; export PLUGINDIR=\"" . $settings['pluginDirectory'] . "\"; ";
	$cloneCmd = $envPrefix . "$fppDir/scripts/install_plugin $plugin \"$srcURL\" \"$branch\" \"$sha\"";
	if ($streaming) {
		system($cloneCmd, $return_val);
	} else {
		exec($cloneCmd, $o, $return_val);
		unset($o);
	}
	if ($return_val != 0) {
		echo "\nERROR: failed to clone plugin '$plugin'.\n";
		return false;
	}

	// Determine the authoritative pluginInfo: the repo's own pluginInfo.json if
	// it ships one, otherwise the one fetched from infoURL. Defer writing the
	// fetched copy / creating the linkName symlink until after the package gate
	// so a refusal leaves nothing behind.
	$infoFile = $settings['pluginDirectory'] . '/' . $plugin . '/pluginInfo.json';
	$fetchedInfo = null;
	$data = null;
	if (file_exists($infoFile)) {
		$data = json_decode(file_get_contents($infoFile), true);
	} else if ($infoURL !== '') {
		$fetchedInfo = FetchURLWithGitHubCredentials($infoURL);
		$data = json_decode($fetchedInfo, true);
	}
	if (!is_array($data)) {
		$data = $pluginInfo;
	}
	$deps = (isset($data['dependencies']) && is_array($data['dependencies'])) ? $data['dependencies'] : null;

	// Authoritative package gate: the cloned repo's own pluginInfo.json may
	// declare packages the posted info did not. Same rule -- refuse on a
	// platform without apt, and remove the partial clone.
	if (DepsRequirePackages($deps) && !AptAvailable()) {
		$pkgs = implode(', ', $deps['packages']);
		$plat = isset($settings['Platform']) ? $settings['Platform'] : 'this platform';
		echo "\nERROR: '$repoName' requires system packages ($pkgs) but $plat does not support system packages. Refusing to install.\n";
		CleanupPartialPluginInstall($plugin);
		return false;
	}

	// Install is going ahead: commit the fetched pluginInfo.json + linkName.
	$linkName = null;
	if ($fetchedInfo !== null) {
		file_put_contents($infoFile, $fetchedInfo);
		if (isset($data['linkName'])) {
			$linkName = $data['linkName'];
			exec("cd " . $settings['pluginDirectory'] . " && ln -s " . $plugin . " " . escapeshellarg($linkName), $o, $rv);
			unset($o);
		}
	}

	// Resolve declared dependencies BEFORE the plugin's own install script. If a
	// required dependency cannot be installed, refuse and clean up rather than
	// run the plugin's install script against missing prerequisites.
	if ($deps !== null) {
		if (!ResolvePluginDependencies($deps, $repoName, $visited, $stream, $depth)) {
			echo "\nERROR: refusing to complete install of '$plugin' -- a required dependency could not be installed.\n";
			CleanupPartialPluginInstall($plugin, $linkName);
			return false;
		}
	}

	// Finally, run the plugin's own install script. It was deferred above (via
	// FPP_SKIP_INSTALL_SCRIPT) so dependencies are in place first; run it now the
	// same way install_plugin / UpgradePlugin do -- fpp_install.sh with
	// FPPDIR/SRCDIR, preferring scripts/ with the legacy repo-root fallback.
	$pluginBase = $settings['pluginDirectory'] . '/' . $plugin;
	$installScript = $pluginBase . '/scripts/fpp_install.sh';
	if (!is_executable($installScript)) {
		$installScript = $pluginBase . '/fpp_install.sh';
	}
	if (is_executable($installScript)) {
		$runCmd = $SUDO . " " . $installScript . " FPPDIR=" . $fppDir . " SRCDIR=" . $fppDir . "/src";
		if ($streaming) {
			system($runCmd, $return_val);
		} else {
			exec($runCmd, $o, $return_val);
			unset($o);
		}
	}

	echo "\nInstalled plugin '$plugin'.\n";
	return true;
}

/**
 * Resolves a plugin's dependencies block: system packages (apt, ref-counted to
 * the owning plugin), script-repository scripts ("Category/file"), and other
 * plugins (installed transitively). Packages are installed first, then scripts,
 * then dependency plugins. Returns false if a *required* dependency (a declared
 * package, or a dependency plugin) could not be installed, so the caller can
 * refuse the whole install; script-repository entries are treated as soft.
 */
function ResolvePluginDependencies($deps, $ownerRepo, &$visited, $stream, $depth)
{
	global $settings, $fppDir, $SUDO;
	$streaming = (isset($stream) && $stream != "false");
	$ok = true;

	// --- packages (apt) ---
	if (isset($deps['packages']) && is_array($deps['packages']) && count($deps['packages'])) {
		echo "\n=== Installing package dependencies for $ownerRepo ===\n";
		flush();
		// Refresh package lists once for the whole batch.
		AptGetUpdate();
		foreach ($deps['packages'] as $pkg) {
			if (is_string($pkg) && $pkg !== '') {
				if (!InstallSystemPackage($pkg, $ownerRepo, false)) {
					$ok = false;
				}
			}
		}
	}

	// --- scripts (script repository "Category/file") ---
	if (isset($deps['scripts']) && is_array($deps['scripts']) && count($deps['scripts'])) {
		echo "\n=== Installing script dependencies for $ownerRepo ===\n";
		flush();
		foreach ($deps['scripts'] as $entry) {
			if (!is_string($entry) || strpos($entry, '/') === false) {
				echo "\nSkipping malformed script dependency '$entry' (expected 'Category/file').\n";
				continue;
			}
			list($category, $file) = explode('/', $entry, 2);
			if (!preg_match('#^[A-Za-z0-9._ -]+$#', $category) || !preg_match('#^[A-Za-z0-9._ /-]+$#', $file)) {
				echo "\nSkipping script dependency with unsafe characters: '$entry'.\n";
				continue;
			}
			echo "\nInstalling script '$entry'...\n";
			flush();
			$cmd = $SUDO . " $fppDir/scripts/installScript " . escapeshellarg($category) . " " . escapeshellarg($file);
			if ($streaming) {
				system($cmd);
			} else {
				exec($cmd, $o);
				unset($o);
			}
		}
	}

	// --- dependency plugins (transitive) ---
	if (isset($deps['plugins']) && is_array($deps['plugins']) && count($deps['plugins'])) {
		echo "\n=== Installing plugin dependencies for $ownerRepo ===\n";
		flush();
		foreach ($deps['plugins'] as $depName) {
			if (!is_string($depName) || $depName === '') {
				continue;
			}
			if (isset($visited[$depName])) {
				continue;
			}
			if (file_exists($settings['pluginDirectory'] . '/' . escapeshellcmd($depName))) {
				echo "\nDependency plugin '$depName' is already installed.\n";
				$visited[$depName] = true;
				continue;
			}
			$depInfo = ResolvePluginInfoByName($depName);
			if ($depInfo === null) {
				echo "\nERROR: could not resolve dependency plugin '$depName' from pluginList.json (skipping).\n";
				continue;
			}
			$ver = SelectPluginVersion($depInfo);
			if ($ver !== null) {
				$depInfo['branch'] = $ver['branch'];
				$depInfo['sha'] = $ver['sha'];
			}
			if (!InstallPluginFromInfo($depInfo, $visited, $stream, $depth + 1)) {
				echo "\nERROR: dependency plugin '$depName' could not be installed.\n";
				$ok = false;
			}
		}
	}

	return $ok;
}

/**
 * Resolves a plugin repoName to its pluginInfo structure by consulting the
 * official pluginList.json in FalconChristmas/fpp-data. Only names present in
 * that list can be installed as dependencies (arbitrary URLs are rejected).
 * Returns the decoded pluginInfo (with infoURL set) or null.
 */
function ResolvePluginInfoByName($repoName)
{
	static $pluginList = null;
	if ($pluginList === null) {
		$listJSON = FetchURLWithGitHubCredentials('https://raw.githubusercontent.com/FalconChristmas/fpp-data/master/pluginList.json');
		$decoded = json_decode($listJSON, true);
		$pluginList = (is_array($decoded) && isset($decoded['pluginList']) && is_array($decoded['pluginList'])) ? $decoded['pluginList'] : array();
	}

	$infoURL = null;
	foreach ($pluginList as $entry) {
		if (is_array($entry) && count($entry) >= 2 && $entry[0] === $repoName) {
			$infoURL = $entry[1];
			break;
		}
	}
	if ($infoURL === null) {
		return null;
	}

	$infoJSON = FetchURLWithGitHubCredentials($infoURL);
	$info = json_decode($infoJSON, true);
	if (!is_array($info)) {
		return null;
	}
	$info['infoURL'] = $infoURL;
	return $info;
}

/**
 * Picks the branch/sha to install for a dependency plugin based on the running
 * FPP version and platform. PHP port of the version-window selection in
 * www/plugins.php (LoadPlugin). Falls back to the first version entry when none
 * match. Returns array('branch'=>..., 'sha'=>...) or null.
 */
function SelectPluginVersion($pluginInfo)
{
	global $settings;
	if (!isset($pluginInfo['versions']) || !is_array($pluginInfo['versions']) || count($pluginInfo['versions']) === 0) {
		return null;
	}
	$triplet = getFPPVersionTriplet();
	$versions = $pluginInfo['versions'];
	$compatible = -1;
	foreach ($versions as $i => $v) {
		$min = isset($v['minFPPVersion']) ? $v['minFPPVersion'] : '0';
		$max = isset($v['maxFPPVersion']) ? $v['maxFPPVersion'] : '';
		$openMax = ($max === '0' || $max === '0.0' || $max === '');
		$platformsOk = true;
		if (isset($v['platforms']) && is_array($v['platforms'])) {
			$platformsOk = isset($settings['Platform']) && in_array($settings['Platform'], $v['platforms']);
		}
		$minOk = (ComparePluginFPPVersions($min, $triplet) <= 0);
		$maxOk = $openMax || (ComparePluginFPPVersions($max, $triplet) > 0);
		if ($minOk && $maxOk && $platformsOk) {
			$compatible = $i; // last matching entry wins, matching the JS logic
		}
	}
	if ($compatible < 0) {
		$compatible = 0; // fall back to first entry so the dependency still installs
	}
	$v = $versions[$compatible];
	return array(
		'branch' => isset($v['branch']) && $v['branch'] !== '' ? $v['branch'] : 'master',
		'sha' => isset($v['sha']) ? $v['sha'] : ''
	);
}

// PHP port of versionToNumber() in www/js/fpp.js -- turns a version string into
// a comparable integer.
function PluginVersionToNumber($version)
{
	$version = (string) $version;
	if (strlen($version) > 0 && $version[0] === 'v') {
		$version = substr($version, 1);
	}
	$dash = strpos($version, '-');
	if ($dash !== false) {
		$version = substr($version, 0, $dash);
	}
	$parts = explode('.', $version);
	while (count($parts) < 3) {
		$parts[] = '0';
	}
	$number = 0;
	for ($x = 0; $x < 3; $x++) {
		$val = intval($parts[$x]);
		if ($val >= 9990) {
			return $number * 10000 + 9999;
		} else if ($val > 99) {
			$val = 99;
		}
		$number = $number * 100 + $val;
	}
	return $number;
}

// Returns -1/0/1 comparing two FPP version strings (port of CompareFPPVersions).
function ComparePluginFPPVersions($a, $b)
{
	$a = PluginVersionToNumber($a);
	$b = PluginVersionToNumber($b);
	return ($a <=> $b);
}

/**
 * Get plugin information
 *
 * Get `pluginInfo.json` for installed plugin `{RepoName}`. An additional
 * `updatesAvailable` field indicates whether the plugin has commits that
 * have been fetched but not yet merged.
 *
 * @route GET /api/plugin/{RepoName}
 * @response 200 Plugin information
 * ```json
 * {
 *   "repoName": "fpp-matrixtools",
 *   "name": "MatrixTools",
 *   "author": "Chris Pinkham (CaptainMurdoch)",
 *   "srcURL": "https://github.com/cpinkham/fpp-matrixtools.git",
 *   "updatesAvailable": 0,
 *   "versions": [
 *     {
 *       "minFPPVersion": 0,
 *       "maxFPPVersion": 0,
 *       "branch": "master",
 *       "sha": ""
 *     }
 *   ]
 * }
 * ```
 */
function GetPluginInfo()
{
	global $settings;

	$plugin = params('RepoName');
	$infoFile = $settings['pluginDirectory'] . '/' . $plugin . '/pluginInfo.json';

	if (file_exists($infoFile)) {
		$json = file_get_contents($infoFile);
		$result = json_decode($json, true);
		$result['Status'] = 'OK';
		$result['updatesAvailable'] = PluginHasUpdates($plugin);

		return json($result);
	}

	$result = array();
	$result['Status'] = 'Error';

	if (!file_exists($settings['pluginDirectory'] . '/' . $plugin))
		$result['Message'] = 'Plugin is not installed';
	else
		$result['Message'] = 'pluginInfo.json does not exist';

	return json($result);
}

/**
 * Uninstall plugin
 *
 * Uninstall plugin {RepoName}.
 *
 * @route DELETE /api/plugin/{RepoName}
 * @response 200 Plugin uninstalled
 * ```json
 * {"Status": "OK", "Message": ""}
 * ```
 */
function UninstallPlugin()
{
	global $settings, $fppDir, $SUDO, $_REQUEST;
	$result = array();
	$stream = $_REQUEST['stream'];

	$plugin = params('RepoName');

	if (file_exists($settings['pluginDirectory'] . '/' . $plugin)) {
		$infoFile = $settings['pluginDirectory'] . '/' . $plugin . '/pluginInfo.json';
		if (file_exists($infoFile)) {
			$info = file_get_contents($infoFile);

			$data = json_decode($info, true);

			if (isset($data['linkName']))
				exec("rm " . $settings['pluginDirectory'] . "/" . $data['linkName'], $output, $return_val);

			// Drop this plugin's claim on any packages it declared as
			// dependencies. A package is only apt-removed once nothing else
			// (the user or another plugin) still requires it.
			if (isset($data['dependencies']['packages']) && is_array($data['dependencies']['packages'])) {
				if (isset($stream) && $stream != "false") {
					DisableOutputBuffering();
				}
				foreach ($data['dependencies']['packages'] as $pkg) {
					if (is_string($pkg) && $pkg !== '') {
						RemoveSystemPackageRequester($pkg, $plugin);
					}
				}
			}
		}

		if (isset($stream) && $stream != "false") {
			DisableOutputBuffering();
			system("$fppDir/scripts/uninstall_plugin $plugin", $return_val);
		} else {
			exec("export SUDO=\"" . $SUDO . "\"; export PLUGINDIR=\"" . $settings['pluginDirectory'] . "\"; $fppDir/scripts/uninstall_plugin $plugin", $output, $return_val);
			unset($output);
		}

		if ($return_val == 0) {
			if (isset($stream) && $stream != "false") {
				return "\nDone\n";
			}
			$result['Status'] = 'OK';
			$result['Message'] = '';
		} else {
			$result['Status'] = 'Error';
			$result['Message'] = 'Failed to properly uninstall plugin (' . $plugin . ')';
		}
	} else {
		$result['Status'] = 'Error';
		$result['Message'] = 'The plugin (' . $plugin . ') is not installed';
	}

	return json($result);
}

/**
 * Check plugin for updates
 *
 * Check plugin `{RepoName}` for available updates by running `git fetch` in
 * the plugin directory and checking for any unmerged commits.
 *
 * @route POST /api/plugin/{RepoName}/updates
 * @response 200 Update check result
 * ```json
 * {"Status": "OK", "Message": "", "updatesAvailable": 1}
 * ```
 */
function CheckForPluginUpdates()
{
	global $settings, $SUDO;
	$result = array();

	$plugin = params('RepoName');

	$cmd = '(cd ' . $settings['pluginDirectory'] . '/' . $plugin . ' && ' . $SUDO . ' git fetch)';
	exec($cmd, $output, $return_val);

	if ($return_val == 0) {
		$result['Status'] = 'OK';
		$result['Message'] = '';
		$result['updatesAvailable'] = PluginHasUpdates($plugin);
	} else {
		$result['Status'] = 'Error';
		$result['Message'] = 'Could not run git fetch for plugin ' . $plugin;
	}

	return json($result);
}

/**
 * Update plugin
 *
 * Pull in git updates for plugin `{RepoName}`. Supports an optional
 * `?stream=true` query parameter for streaming output.
 *
 * @route GET /api/plugin/{RepoName}/upgrade
 * @route POST /api/plugin/{RepoName}/upgrade
 * @response 200 Plugin upgraded
 * ```json
 * {"Status": "OK", "Message": ""}
 * ```
 */
function UpgradePlugin()
{
	global $settings, $SUDO, $_REQUEST, $fppDir;
	$result = array();

	$plugin = params('RepoName');
	$stream = $_REQUEST['stream'];

	// After the git pull, run the plugin's optional upgrade script if it has
	// one (for plugins whose artifacts live outside git, e.g. prebuilt
	// binaries on a release — a re-run of the install script may consider
	// the cached artifact current); otherwise re-run the install script as
	// before.
	$post_pull_script = $settings['pluginDirectory'] . '/' . $plugin . '/scripts/fpp_upgrade.sh';
	if (!file_exists($post_pull_script)) {
		$post_pull_script = $settings['pluginDirectory'] . '/' . $plugin . '/scripts/fpp_install.sh';
	}
	if (!file_exists($post_pull_script)) {
		$post_pull_script = $settings['pluginDirectory'] . '/' . $plugin . '/fpp_install.sh';
	}

	if (isset($stream) && $stream != "false") {
		DisableOutputBuffering();
		$cmd = '(cd ' . $settings['pluginDirectory'] . '/' . $plugin . ' && ' . $SUDO . ' git pull)';
		system($cmd, $return_val);
		if ($return_val != 0) {
			$cmd = '(cd ' . $settings['pluginDirectory'] . '/' . $plugin . ' && ' . $SUDO . ' git clean -fd && ' . $SUDO . ' git pull)';
			system($cmd, $return_val);
		}
		if (file_exists($post_pull_script)) {
			echo "Running " . $post_pull_script . "\n";
			system($SUDO . "  FPPDIR=" . $fppDir . " SRCDIR=" . $fppDir . "/src " . $post_pull_script, $return_val);
		}
		return "\nDone\n";
	}
	$cmd = '(cd ' . $settings['pluginDirectory'] . '/' . $plugin . ' && ' . $SUDO . ' git pull)';
	exec($cmd, $output, $return_val);
	if ($return_val != 0) {
		$cmd = '(cd ' . $settings['pluginDirectory'] . '/' . $plugin . ' && ' . $SUDO . ' git clean -fd && ' . $SUDO . ' git pull)';
		exec($cmd, $output, $return_val);
	}
	if (file_exists($post_pull_script)) {
		exec($SUDO . "  FPPDIR=" . $fppDir . " SRCDIR=" . $fppDir . "/src " . $post_pull_script, $return_val);
	}

	if ($return_val == 0) {
		$result['Status'] = 'OK';
		$result['Message'] = '';
	} else {
		$result['Status'] = 'Error';
		$result['Message'] = 'Could not run git pull for plugin ' . $plugin;
	}

	return json($result);
}

// Helper functions

/**
 * Injects GitHub credentials (username + Personal Access Token) into a
 * GitHub HTTPS URL so `git clone` and `curl` can authenticate against
 * private repositories.
 *
 * @param string $url GitHub HTTPS URL to inject credentials into.
 * @return string|false Modified URL on success, or false if credentials are not
 *                      configured or the URL is not a recognized GitHub URL.
 */
function InjectGitHubCredentials($url)
{
	global $settings;

	$user = isset($settings['gitHubUser']) ? trim($settings['gitHubUser']) : '';
	$pat = isset($settings['gitHubPAT']) ? trim($settings['gitHubPAT']) : '';

	if ($user === '' || $pat === '')
		return false;

	// Only inject into github.com / raw.githubusercontent.com URLs to avoid
	// leaking credentials to unrelated hosts.
	if (!preg_match('#^https://(github\.com|raw\.githubusercontent\.com|api\.github\.com)/#i', $url))
		return $url;

	return preg_replace('#^https://#i', 'https://' . rawurlencode($user) . ':' . rawurlencode($pat) . '@', $url, 1);
}

/**
 * Fetches the contents of a URL using GitHub credentials when available.
 * Falls back to `file_get_contents` when credentials are not configured.
 * `raw.githubusercontent.com` requires an `Authorization: token <PAT>`
 * header rather than HTTP Basic auth for private content. Temporary
 * share-link tokens (`?token=GHSAT...`) are stripped and replaced with
 * the configured PAT.
 *
 * @param string $url URL to fetch.
 * @return string|false Response body on success, or false on failure.
 */
function FetchURLWithGitHubCredentials($url)
{
	global $GitHubFetchLastError;
	$GitHubFetchLastError = '';
	global $settings;

	$user = isset($settings['gitHubUser']) ? trim($settings['gitHubUser']) : '';
	$pat = isset($settings['gitHubPAT']) ? trim($settings['gitHubPAT']) : '';
	$haveCreds = ($user !== '' && $pat !== '');

	// Only treat as a GitHub URL (and apply credentials/normalization) when
	// the host is one of the known GitHub hosts.
	$isGitHub = (bool) preg_match('#^https://(github\.com|raw\.githubusercontent\.com|api\.github\.com)/#i', $url);

	if ($isGitHub && $haveCreds) {
		// Strip GitHub's temporary "Raw" share-link token (e.g. ?token=GHSAT...)
		// since we'll authenticate with the configured PAT instead.
		$fetchUrl = preg_replace('/([?&])token=GHSAT[^&]*(&|$)/i', '$1', $url);
		$fetchUrl = preg_replace('/[?&]$/', '', $fetchUrl);
		$hadGhsatToken = ($fetchUrl !== $url);

		$attempts = array($fetchUrl);

		// Build a fallback URL using the GitHub Contents API which is the
		// most reliable way to fetch a file from a private repo with a PAT
		// (raw.githubusercontent.com can return 404 even with a valid PAT
		// in some configurations -- particularly with fine-grained tokens).
		if (preg_match('#^https://raw\.githubusercontent\.com/([^/]+)/([^/]+)/([^/]+)/(.+)$#i', $fetchUrl, $m)) {
			$owner = $m[1];
			$repo = $m[2];
			$ref = $m[3];
			$path = $m[4];
			// raw URLs sometimes use "refs/heads/<branch>"
			if (strpos($ref, 'refs') === 0 && isset($m[4])) {
				// path starts with "heads/<branch>/<file>" -- handle "refs/heads/<branch>/<path>"
				if (preg_match('#^heads/([^/]+)/(.+)$#', $path, $rm)) {
					$ref = $rm[1];
					$path = $rm[2];
				}
			}
			$apiUrl = 'https://api.github.com/repos/' . $owner . '/' . $repo . '/contents/' . $path . '?ref=' . rawurlencode($ref);
			$attempts[] = $apiUrl;
		}

		$lastCode = 0;
		$lastBody = '';
		$lastErr = '';
		foreach ($attempts as $tryUrl) {
			if (function_exists('curl_init')) {
				$ch = curl_init($tryUrl);
				curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
				curl_setopt($ch, CURLOPT_FOLLOWLOCATION, true);
				curl_setopt($ch, CURLOPT_USERAGENT, 'FPP-PluginManager');
				curl_setopt($ch, CURLOPT_HTTPHEADER, array(
					'Authorization: token ' . $pat,
					'Accept: application/vnd.github.raw, application/json, */*',
					'X-GitHub-Api-Version: 2022-11-28',
				));
				$data = curl_exec($ch);
				$httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
				$curlErr = curl_error($ch);
				curl_close($ch);
				if ($data !== false && $httpCode >= 200 && $httpCode < 400) {
					return $data;
				}
				$lastCode = $httpCode;
				$lastBody = is_string($data) ? $data : '';
				$lastErr = $curlErr;
			} else {
				$ctx = stream_context_create(array(
					'http' => array(
						'header' => "Authorization: token " . $pat . "\r\n" .
							"User-Agent: FPP-PluginManager\r\n" .
							"Accept: application/vnd.github.raw, application/json, */*\r\n",
						'follow_location' => 1,
						'ignore_errors' => 1,
					),
				));
				$data = @file_get_contents($tryUrl, false, $ctx);
				if ($data !== false && $data !== '') {
					return $data;
				}
				$lastBody = '';
				$lastErr = 'file_get_contents failed';
			}
		}

		// PAT authentication failed. If the original URL contained a GitHub share-link
		// token (?token=GHSAT...) try it as a last resort — server-side requests are not
		// subject to CORS, so the share-link token can work here even when the browser
		// fetch failed. This handles the case where the PAT is missing or expired but
		// the URL was freshly copied from GitHub.
		if ($hadGhsatToken) {
			$ghsatData = @file_get_contents($url);
			if ($ghsatData !== false && $ghsatData !== '') {
				return $ghsatData;
			}
		}

		$patHint = ($lastCode === 401 || $lastCode === 403)
			? ' The configured GitHub Personal Access Token may be invalid or expired — check the Developer settings page.'
			: '';
		$GitHubFetchLastError = 'HTTP ' . $lastCode .
			($lastErr !== '' ? ' (' . $lastErr . ')' : '') .
			($lastBody !== '' ? ': ' . trim(substr($lastBody, 0, 200)) : '') .
			$patHint;
		return false;
	}

	return @file_get_contents($url);
}

/**
 * Get plugin info from URL
 *
 * Server-side proxy for fetching a `pluginInfo.json` from a remote URL.
 * Used to retrieve plugin repository info without CORS issues, and to
 * authenticate against private GitHub repositories using credentials
 * configured on the Developer settings page.
 *
 * @route POST /api/plugin/fetchInfo
 * @body {"url": "https://example.com/pluginInfo.json", "useCredentials": 1}
 * @response 200 Plugin info fetched from remote URL
 * ```json
 * {}
 * ```
 */
function FetchPluginInfoProxy()
{
	$body = '';
	$fp = fopen('php://input', 'r');
	while ($d = fread($fp, 1024 * 16)) {
		$body .= $d;
	}
	fclose($fp);

	$req = json_decode($body, true);
	$url = isset($req['url']) ? $req['url'] : '';
	$useCreds = isset($req['useCredentials']) && $req['useCredentials'];

	if ($url === '' || !preg_match('#^https://#i', $url)) {
		return json(array('Status' => 'Error', 'Message' => 'Invalid URL'));
	}

	if ($useCreds) {
		$user = isset($GLOBALS['settings']['gitHubUser']) ? trim($GLOBALS['settings']['gitHubUser']) : '';
		$pat = isset($GLOBALS['settings']['gitHubPAT']) ? trim($GLOBALS['settings']['gitHubPAT']) : '';
		if ($user === '' || $pat === '') {
			return json(array('Status' => 'Error', 'Message' => 'GitHub user name and/or Personal Access Token are not configured on the Developer settings page.'));
		}
		$data = FetchURLWithGitHubCredentials($url);
	} else {
		$data = file_get_contents($url);
	}

	if ($data === false || $data === null || $data === '') {
		global $GitHubFetchLastError;
		$detail = (isset($GitHubFetchLastError) && $GitHubFetchLastError !== '') ? ' [' . $GitHubFetchLastError . ']' : '';
		return json(array('Status' => 'Error', 'Message' => 'Failed to fetch pluginInfo.json from ' . $url . $detail));
	}

	$decoded = json_decode($data, true);
	if (!is_array($decoded)) {
		$snippet = trim(substr($data, 0, 200));
		return json(array('Status' => 'Error', 'Message' => 'Response from ' . $url . ' was not valid JSON. First bytes: ' . $snippet));
	}

	return json($decoded);
}

/**
 * Checks whether the installed plugin has updates available: commits that
 * have been fetched but not yet merged into the local branch, or — for
 * plugins that distribute artifacts outside of git (e.g. prebuilt binaries
 * attached to a release) — updates reported by the plugin's own optional
 * update-check script.
 *
 * A plugin may provide scripts/fpp_update_check.sh. It is run with
 * FPPDIR/SRCDIR set (like fpp_install.sh); the last line of its stdout must
 * be "1" if an update is available or "0" if not. A non-zero exit status
 * means "could not check" and is ignored. The script's answer is OR'd with
 * the git check, so repo commits are still detected for such plugins.
 *
 * @param string $plugin Plugin directory name (repo name).
 * @return int 1 if updates are available, 0 otherwise.
 */
function PluginHasUpdates($plugin)
{
	global $settings, $fppDir;
	$output = '';

	$cmd = '(cd ' . $settings['pluginDirectory'] . '/' . $plugin . ' && git log $(git rev-parse --abbrev-ref HEAD)..origin/$(git rev-parse --abbrev-ref HEAD))';
	exec($cmd, $output, $return_val);

	if (($return_val == 0) && !empty($output))
		return 1;

	$check_script = $settings['pluginDirectory'] . '/' . $plugin . '/scripts/fpp_update_check.sh';
	if (file_exists($check_script)) {
		unset($output);
		exec("FPPDIR=" . $fppDir . " SRCDIR=" . $fppDir . "/src " . $check_script, $output, $return_val);
		if (($return_val == 0) && !empty($output) && (trim(end($output)) == '1'))
			return 1;
	}

	return 0;
}

/**
 * Get setting from plugin
 *
 * Returns the value of setting `{SettingName}` from plugin `{RepoName}`.
 *
 * @route GET /api/plugin/{RepoName}/settings/{SettingName}
 * @response 200 Plugin setting value
 * ```json
 * {"status": "OK", "SettingName": "SettingValue"}
 * ```
 */
function PluginGetSetting()
{
	$setting = params("SettingName");
	$plugin = params("RepoName");

	$value = ReadSettingFromFile($setting, $plugin);

	$result = array("status" => "OK");
	$result[$setting] = $value;

	return json($result);

}

/**
 * Set setting for plugin
 *
 * Sets `{SettingName}` for plugin `{RepoName}` and returns the updated value.
 *
 * @route POST /api/plugin/{RepoName}/settings/{SettingName}
 * @route PUT /api/plugin/{RepoName}/settings/{SettingName}
 * @body SettingValue
 * @response 200 Plugin setting updated
 * ```json
 * {"status": "OK", "SettingName": "SettingValue"}
 * ```
 */
function PluginSetSetting()
{

	$setting = params("SettingName");
	$plugin = params("RepoName");
	$value = file_get_contents('php://input');

	WriteSettingToFile($setting, $value, $plugin);

	return PluginGetSetting();
}

?>