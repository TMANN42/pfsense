#!/usr/local/bin/php-cgi -q
<?php
/*
 * ecl.php
 *
 * Copyright (c) 2010-2013 BSD Perimeter
 * Copyright (c) 2013-2016 Electric Sheep Fencing
 * Copyright (c) 2014-2025 Rubicon Communications, LLC (Netgate)
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

require_once("globals.inc");
require_once("functions.inc");
require_once("config.lib.inc");
require_once("config.inc");

$debug = false;

function get_boot_disk() {
	global $g, $debug;
	$disk = exec("/sbin/mount | /usr/bin/grep \"on / \" | /usr/bin/cut -d'/' -f3 | /usr/bin/cut -d' ' -f1");
	return $disk;
}

function get_swap_disks() {
	exec("/usr/sbin/swapinfo | /usr/bin/sed '/^\/dev/!d; s,^/dev/,,; s, .*\$,,'", $disks);
	return $disks;
}

function get_disk_slices($disk) {
	global $g, $debug;

	if (str_starts_with($disk, 'cd')) {
		$slices[] = $disk;
	} else {
		$slices = glob("/dev/" . $disk . "[ps]*");
		$slices = str_replace("/dev/", "", $slices);
	}
	return $slices;
}

function get_disks() {
	global $g, $debug;
	$disks_array = array();
	$disks_s = explode(" ", get_single_sysctl("kern.disks"));
	foreach ($disks_s as $disk) {
		/* Ignore the flash devices (ARM). */
		if (strstr($disk, "flash")) {
			continue;
		}
		if (trim($disk)) {
			$disks_array[] = $disk;
		}
	}
	return $disks_array;
}

function discover_config($mountpoint) {
	global $g, $debug;
	/* List of locations to check. Requires trailing slash.
	 * See https://redmine.pfsense.org/issues/9066 */
	$locations_to_check = array("/", "/config/");
	foreach ($locations_to_check as $ltc) {
		$tocheck = "/tmp/mnt/cf{$ltc}config.xml";
		if ($debug) {
			echo "\nChecking for $tocheck";
			if (file_exists($tocheck)) {
				echo " -> found!";
			}
		}
		if (file_exists($tocheck)) {
			return $tocheck;
		}
	}
	return "";
}

function test_config($file_location) {
	global $g, $debug;
	if (!$file_location) {
		return;
	}
	// config.xml was found.  ensure it is sound.
	$root_obj = trim("<{$g['xml_rootobj']}>");
	$enc_root_obj = "---- BEGIN config.xml ----";
	$xml_file_head = exec("/usr/bin/head -2 " . escapeshellarg($file_location) . " | /usr/bin/tail -n1");
	$enc_file_head = exec("/usr/bin/head -1 " . escapeshellarg($file_location));
	if ($debug) {
		echo "\nroot obj  = $root_obj";
		echo "\nfile head = $xml_file_head";
	}
	if ($xml_file_head == $root_obj) {
		// Now parse config to make sure
		$config_status = config_validate($file_location);
		if ($config_status) {
			return 'xml';
		}
	} elseif ($enc_file_head == $enc_root_obj) {
		return 'enc';
	}
	return false;
}

// Probes all disks looking for config.xml
function find_config_xml() {
	global $g, $debug;
	$disks = get_disks();
	// Safety check.
	if (!is_array($disks)) {
		return;
	}
	$boot_disk = get_boot_disk();
	$swap_disks = get_swap_disks();
	exec("/bin/mkdir -p /tmp/mnt/cf");
	foreach ($disks as $disk) {
		$slices = get_disk_slices($disk);
		if (is_array($slices)) {
			foreach ($slices as $slice) {
				if ($slice == "") {
					continue;
				}
				if (stristr($slice, $boot_disk)) {
					if ($debug) {
						echo "\nSkipping boot device slice $slice";
					}
					continue;
				}
				if (in_array($slice, $swap_disks)) {
					if ($debug) {
						echo "\nSkipping swap device slice $slice";
					}
					continue;
				}
				echo " $slice";
				// try msdos fs
				if ($debug) {
					echo "\n/sbin/mount -t msdosfs /dev/{$slice} /tmp/mnt/cf 2>/dev/null \n";
				}
				$_out = exec("/sbin/mount -t msdosfs /dev/{$slice} /tmp/mnt/cf 2>/dev/null", $stdout, $status);
				// try regular fs (ufs)
				if ($_out === false || $status != 0) {
					if ($debug) {
						echo "\n/sbin/mount /dev/{$slice} /tmp/mnt/cf 2>/dev/null \n";
					}
					$_out = exec("/sbin/mount /dev/{$slice} /tmp/mnt/cf 2>/dev/null", $stdout, $status);
				}
				// try cd9660 (standard CD-ROM format)
				if ($_out === false || $status != 0) {
					if ($debug) {
						echo "\n/sbin/mount -t cd9660 /dev/{$slice} /tmp/mnt/cf 2>/dev/null \n";
					}
					$_out = exec("/sbin/mount -t cd9660 /dev/{$slice} /tmp/mnt/cf 2>/dev/null", $stdout, $status);
				}
				// try udf (common for modern DVDs and some CDs)
				if ($_out === false || $status != 0) {
					if ($debug) {
						echo "\n/sbin/mount -t udf /dev/{$slice} /tmp/mnt/cf 2>/dev/null \n";
					}
					$_out = exec("/sbin/mount -t udf /dev/{$slice} /tmp/mnt/cf 2>/dev/null", $stdout, $status);
				}
				$mounted = trim(exec("/sbin/mount | /usr/bin/grep -v grep | /usr/bin/grep '/tmp/mnt/cf' | /usr/bin/wc -l"));
				if ($debug) {
					echo "\nmounted: $mounted ";
				}
				if ($status == 0 && intval($mounted) > 0) {
					// Item was mounted - look for config.xml file
					$config_location = discover_config($slice);
					if ($config_location) {
						$config_type = test_config($config_location);
						if ($config_type) {
							if (($config_type == 'enc') && !password_prompt($config_location)) {
								exec("/sbin/umount /tmp/mnt/cf");
								rmdir("/tmp/mnt/cf");
								exit;
							} elseif ($config_type == 'xml') {
								// We have a valid configuration.  Install it.
								echo " -> found config.xml\n";
							}
							echo "Backing up old configuration...\n";
							backup_config();
							echo "Restoring [{$slice}] {$config_location}...\n";
							restore_backup($config_location);
							if (file_exists('/cf/conf/trigger_initial_wizard')) {
								echo "First boot after install, setting flag for package sync and disabling wizard...\n";
								touch('/cf/conf/needs_package_sync');
								@unlink('/cf/conf/trigger_initial_wizard');
							}
							echo "Cleaning up...\n";
							exec("/sbin/umount /tmp/mnt/cf");
							rmdir("/tmp/mnt/cf");
							exit;
						}
					}
					exec("/sbin/umount /tmp/mnt/cf");
				}
			}
		}
	}
	rmdir("/tmp/mnt/cf");
}

function password_prompt($config_location) {
	echo " -> found encrypted config.xml\n";

	$configtxt = file_get_contents($config_location);

	if (tagfile_deformat($configtxt, $configtxt, "config.xml")) {
		$fp = fopen('php://stdin', 'r');
		$read = array($fp);
		$write = $except = array();
		$timeout = 60; // skip after 60s of inactivity
		do {
			echo gettext("Enter the password to decrypt config.xml, or press <ENTER> to skip:") . "\n";
			if (stream_select($read, $write, $except, $timeout)) {
				$decrypt_password = chop(fgets($fp));
			} else {
				echo gettext("Input timeout, skipping config.xml restore...") . "\n";
				return false;
			}
			$data = decrypt_data($configtxt, $decrypt_password);
			if (!empty($decrypt_password)) {
				$decrypted_data = decrypt_data($configtxt, $decrypt_password);
				if (empty($decrypted_data)) {
					echo gettext("Invalid password entered. Please try again.") . "\n";
				}
			}
		} while (!empty($decrypt_password) && empty($data));
		fclose($fp);
	}

	if (!empty($data)) {
		echo gettext("Config.xml unlocked.") . "\n";
		file_put_contents($config_location, $decrypted_data);
		return true;
	} else {
		echo gettext("Skipping config.xml restore...") . "\n";
		return false;
	}
}

echo "External config loader 1.0 is now starting...";
find_config_xml();
echo "\n";

?>
