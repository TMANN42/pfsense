<?php
/*
 * alias-utils.inc
 *
 * part of pfSense (https://www.pfsense.org)
 * Copyright (c) 2004-2013 BSD Perimeter
 * Copyright (c) 2013-2016 Electric Sheep Fencing
 * Copyright (c) 2014-2025 Rubicon Communications, LLC (Netgate)
 * All rights reserved.
 *
 * originally based on m0n0wall (http://m0n0.ch/wall)
 * Copyright (c) 2003-2004 Manuel Kasper <mk@neon1.net>.
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

require_once("config.gui.inc");
require_once("functions.inc");
require_once("filter.inc");
require_once("shaper.inc");
require_once("openvpn.inc");

global $alias_types;
$alias_types = array(
	'host'	=> gettext("Host(s)"),
	'network' => gettext("Network(s)"),
	'port' => gettext("Port(s)"),
	'url' => gettext("URL (IPs)"),
	'url_ports' => gettext("URL (Ports)"),
	'urltable' => gettext("URL Table (IPs)"),
	'urltable_ports' => gettext("URL Table (Ports)"),
);

global $singular_types;
$singular_types = array(
	'host'	=> gettext("host"),
	'network' => gettext("network"),
	'port' => gettext("port"),
	'url' => gettext("URL (IP)"),
	'url_ports' => gettext("URL (Port)"),
	'urltable' => gettext("URL Table (IP)"),
	'urltable_ports' => gettext("URL Table (Port)"),
);

function deleteAlias($id, $apply = false) {
	/* make sure rule is not being referenced by any nat or filter rules */
	$delete_error = "";
	if (!is_numericint($id)) {
		return $delete_error;
	}
	$is_alias_referenced = false;
	$referenced_by = array();
	$alias = config_get_path("aliases/alias/{$id}");
	$section_fields = [
		// Firewall rules
		'filter/rule' => [
			'source/address',
			'destination/address',
			'destination/port'
		],
		// NAT Rules
		'nat/rule' => [
			'source/address',
			'source/port',
			'destination/address',
			'destination/port',
			'target',
			'local-port'
		],
		// NAT 1:1 Rules
		'nat/onetoone' => [
			'destination/address'
		],
		// NAT Outbound Rules
		'nat/outbound/rule' => [
			'source/network',
			'sourceport',
			'destination/network',
			'dstport',
			'target'
		],
		// Alias in an alias
		'aliases/alias' => [
			'address'
		],
		// Static routes
		'staticroutes/route' => [
			'network'
		],
		// OpenVPN
		'openvpn/openvpn-server' => [
			'tunnel_network',
			'tunnel_networkv6',
			'local_network',
			'local_networkv6',
			'remote_network',
			'remote_networkv6',
			'tunnel_network',
			'tunnel_networkv6',
			'remote_network',
			'remote_networkv6'
		],
		'openvpn/openvpn-csc' => [
			'tunnel_network',
			'tunnel_networkv6',
			'local_network',
			'local_networkv6',
			'remote_network',
			'remote_networkv6'
		],
	];

	foreach ($section_fields as $section => $fields) {
		foreach ($fields as $field) {
			find_alias_reference_by_path($section, $field, $alias['name'], $is_alias_referenced, $referenced_by);
		}
	}

	if ($is_alias_referenced == true) {
		$delete_error = sprintf(gettext("Cannot delete alias. Currently in use by %s."), htmlspecialchars(implode(", ", $referenced_by)));
	} else {
		if (preg_match("/urltable/i", $alias['type'])) {
			// this is a URL table type alias, delete its file as well
			unlink_if_exists("/var/db/aliastables/{$alias['name']}.txt");
		}
		config_del_path('aliases/alias/' . $id);
		if (write_config(gettext("Deleted firewall alias " . $alias['name']))) {
			if (!$apply) {
				mark_subsystem_dirty('aliases');
			} else {
				filter_configure();
			}
		}
	}

	return $delete_error;
}

function openvpnAlias ($id) {
	if (!is_numericint($id)) {
		return false;
	}
	$alarm_message = "";
	$is_alias_referenced = false;
	$referenced_by = array();
	$alias_name = config_get_path('aliases/alias/' . $id . '/name');

	if (config_get_path('aliases/alias/' . $id . '/type') != 'network') {
		return false;
	}

	$section_fields = [
		'openvpn/openvpn-server' => [
			'tunnel_network',
			'tunnel_networkv6',
			'local_network',
			'local_networkv6',
			'remote_network',
			'remote_networkv6'
		],
		'openvpn/openvpn-client' => [
			'tunnel_network',
			'tunnel_networkv6',
			'remote_network',
			'remote_networkv6'
		],
		'openvpn/openvpn-csc' => [
			'tunnel_network',
			'tunnel_networkv6',
			'local_network',
			'local_networkv6',
			'remote_network',
			'remote_networkv6'
		]
	];

	foreach ($section_fields as $section => $fields) {
		foreach ($fields as $field) {
			find_alias_reference_by_path($section, $field, $alias_name, $is_alias_referenced, $referenced_by);
		}
	}

	if ($is_alias_referenced == true) {
		$alarm_message = sprintf(gettext("Alias is in use by OpenVPN. Pressing save will restart instances: %s."), htmlspecialchars(implode(", ", $referenced_by)));
	}

	return $alarm_message;
}

/**
 * Find references to an alias by path.
 *
 * @param  string   $section             Section part of path, slash delimited. The section must identify an indexed array of items 
 * @param  string   $field               Field part of path, slash delimited. The part is appended to each element in the section to search for an alias.
 * @param  string   $origname            Alias name.
 * @param  bool	    $is_alias_referenced Set to true if a match is found.
 * @param  string[] $referenced_by       Array recording descriptions of paths referencing the alias.
 */
function find_alias_reference_by_path($section, $field, $origname, &$is_alias_referenced, &$referenced_by) {
	if (!$origname) {
		return;
	}

	$items = config_get_path($section, []);

	foreach ($items as $id => $item) {
		if (array_get_path($item, $field) == $origname) {
			$is_alias_referenced = true;
			/* This is a bad hack to make rule descriptions more verbose,
			 * ideally the object should be providing its own text presentation,
			 * but our values are arrays at this time
			 */
			if (basename($section) == 'rule') {
				$descr = str_replace('/', ' ', $section) . ' ';
			} else {
				$descr = basename($section) . ' ';
			}
			if (!empty($item['descr'])) {
				$descr .= "'{$item['descr']}'";
			} elseif (!empty($item['description'])) {
				$descr .= "'{$item['description']}'";
			} elseif (!empty($item['name'])) {
				$descr .= "'{$item['name']}'";
			} else {
				$descr .= "id {$id}";
			}

			$referenced_by[] = $descr;
		}
	}
}


function find_alias_reference($section, $field, $origname, &$is_alias_referenced, &$referenced_by) {
	$section_path = join('/', $section);
	$field_path = join('/', $field);

	return (find_alias_reference_by_path($section_path, $field_path, $origname, $is_alias_referenced, $referenced_by));
}

function staticrouteAlias($id) {
	if (!is_numericint($id)) {
		return false;
	}
	$alarm_message = "";
	$is_alias_referenced = false;
	$referenced_by = array();
	$alias_name = config_get_path('aliases/alias/' . $id . '/name');

	if (config_get_path('aliases/alias/' . $id . '/type') != 'host') {
		return false;
	}

	find_alias_reference_by_path('staticroutes/route', 'network', $alias_name, $is_alias_referenced, $referenced_by);

	if ($is_alias_referenced) {
		$alarm_message = sprintf(gettext("Alias is used in static routes. Pressing save will reload static routes: %s."), htmlspecialchars(implode(", ", $referenced_by)));
	}

	return $alarm_message;
}

function alias_same_type($name, $type) {
	foreach (config_get_path('aliases/alias', []) as $alias) {
		if ($name == $alias['name']) {
			if (in_array($type, array("host", "network")) &&
			    in_array($alias['type'], array("host", "network"))) {
				return true;
			}

			if ($type == $alias['type']) {
				return true;
			} else {
				return false;
			}
		}
	}
	return true;
}

function saveAlias($post, $id) {
	global $g, $pconfig, $aliastable, $singular_types;

	$origname = $post['origname'];
	$max_alias_addresses = 5000;

	$data_errors = array();

	$vertical_bar_err_text = gettext("Vertical bars (|) at start or end, or double in the middle of descriptions not allowed. Descriptions have been cleaned. Check and save again.");

	/* input validation */

	$reqdfields = explode(" ", "name");
	$reqdfieldsn = array(gettext("Name"));

	do_local_input_validation($post, $reqdfields, $reqdfieldsn, $data_errors);

	if (!is_validaliasname($post['name'])) {
		$data_errors[] = invalidaliasnamemsg($post['name']);
	}

	/* check for name conflicts */
	foreach (config_get_path('aliases/alias', []) as $key => $alias) {
		if (($alias['name'] == $post['name']) && (empty(config_get_path('aliases/alias/' . $id)) || ($key != $id))) {
			$data_errors[] = gettext("An alias with this name already exists.");
			break;
		}
	}

	/* Check for reserved keyword names */
	foreach (get_pf_reserved() as $rk) {
		if (strcasecmp($rk, $post['name']) == 0) {
			$data_errors[] = sprintf(gettext("Cannot use a reserved keyword as an alias name: %s"), $rk);
		}
	}

	/* Check for captive portal prefix */
	if (substr($post['name'], 0, strlen(CPPREFIX)) == CPPREFIX) {
		$data_errors[] = sprintf(gettext("Cannot use Captive Portal reserved prefix: %s"), CPPREFIX);
	}

	/*
	 * Packages (e.g. tinc) create interface groups, reserve this
	 * namespace pkg_ for them.
	 * One namespace is shared by Interfaces, Interface Groups and Aliases.
	 */
	if (substr($post['name'], 0, 4) == 'pkg_') {
		$data_errors[] = gettext("The alias name cannot start with pkg_");
	}

	/* check for name interface description conflicts */
	foreach (config_get_path('interfaces', []) as $interface) {
		if (strcasecmp($interface['descr'], $post['name']) == 0) {
			$data_errors[] = gettext("An interface description with this name already exists.");
			break;
		}
	}

	/* Is the description already used as an interface group name? */
	foreach (config_get_path('ifgroups/ifgroupentry', []) as $ifgroupentry) {
		if ($ifgroupentry['ifname'] == $post['name']) {
			$data_errors[] = gettext("Sorry, an interface group with this name already exists.");
		}
	}

	/* To prevent infinite loops make sure the alias name does not equal the value. */
	for($i = 0; isset($post['address' . $i]); $i++) {
			if($post['address' . $i] == $post['name']){
				$data_errors[] = gettext("Alias value cannot be the same as the alias name: `" . $post['name'] . " and " . $post['address' . $i] . "`");
			}
	}

	if ($data_errors) {
		$data_errors[] = gettext("There may be additional errors. Fix the noted problems and try again.");
	}
	$alias = array();
	$address = array();
	$final_address_details = array();
	$alias['name'] = $post['name'];
	$alias['type'] = $post['type'];

	if (preg_match("/urltable/i", $post['type'])) {
		$address = array();

		/* item is a url table type */
		if ($post['address0'] && empty($data_errors)) {
			/* fetch down and add in */
			$address[] = $post['address0'];
			$alias['url'] = $post['address0'];
			$alias['updatefreq'] = $post['address_subnet0'] ? $post['address_subnet0'] : 7;
			if (!is_URL($alias['url']) || empty($alias['url'])) {
				$data_errors[] = gettext("A valid URL must be provided.");
			} elseif (!process_alias_urltable($alias['name'], $alias['type'], $alias['url'], 0, true, true)) {
				$data_errors[] = sprintf(gettext("Unable to fetch usable data from URL %s"), htmlspecialchars($alias['url']));
			}
			if ($post["detail0"] <> "") {
				if ((strpos($post["detail0"], "||") === false) && (substr($post["detail0"], 0, 1) != "|") && (substr($post["detail0"], -1, 1) != "|")) {
					$final_address_details[] = $post["detail0"];
				} else {
					/* Remove leading and trailing vertical bars and replace multiple vertical bars with single, */
					/* and put in the output array so the text is at least redisplayed for the user. */
					$final_address_details[] = preg_replace('/\|\|+/', '|', trim($post["detail0"], "|"));
					$data_errors[] = $vertical_bar_err_text;
				}
			} else {
				$final_address_details[] = sprintf(gettext("Entry added %s"), date('r'));
			}
		}
	} elseif (($post['type'] == "url") || ($post['type'] == "url_ports")) {
		$desc_fmt_err_found = false;

		/* item is a url type */
		for ($x = 0; $x < $max_alias_addresses - 1; $x++) {
			if (is_URL($post['address' . $x])) {
				/* fetch down and add in */
				$temp_filename = tempnam($g['tmp_path'] . '/', "alias_import");
				rmdir_recursive($temp_filename);
				$verify_ssl = config_path_enabled('system', 'checkaliasesurlcert');
				mkdir($temp_filename);
				if (!download_file($post['address' . $x], $temp_filename . "/aliases", $verify_ssl)) {
					$data_errors[] = sprintf(gettext("Could not fetch the URL '%s'."), $post['address' . $x]);
					rmdir_recursive($temp_filename);
					continue;
				}

				/* if the item is tar gzipped then extract */
				if (stristr($post['address' . $x], ".tgz") && !process_alias_tgz($temp_filename)) {
					$data_errors[] = sprintf(gettext("Could not unpack tgz from the URL '%s'."), $post['address' . $x]);
					rmdir_recursive($temp_filename);
					continue;
				}

				if (!isset($alias['aliasurl'])) {
					$alias['aliasurl'] = array();
				}

				$alias['aliasurl'][] = $post['address' . $x];
				if ($post["detail{$x}"] <> "") {
					if ((strpos($post["detail{$x}"], "||") === false) && (substr($post["detail{$x}"], 0, 1) != "|") && (substr($post["detail{$x}"], -1, 1) != "|")) {
						$final_address_details[] = $post["detail{$x}"];
					} else {
						/* Remove leading and trailing vertical bars and replace multiple vertical bars with single, */
						/* and put in the output array so the text is at least redisplayed for the user. */
						$final_address_details[] = preg_replace('/\|\|+/', '|', trim($post["detail{$x}"], "|"));
						if (!$desc_fmt_err_found) {
							$data_errors[] = $vertical_bar_err_text;
							$desc_fmt_err_found = true;
						}
					}
				} else {
					$final_address_details[] = sprintf(gettext("Entry added %s"), date('r'));
				}

				if (file_exists("{$temp_filename}/aliases")) {
					$t_address = parse_aliases_file("{$temp_filename}/aliases", $post['type'], 5000);
					if ($t_address == null) {
						/* nothing was found */
						$data_errors[] = sprintf(gettext("A valid URL must be provided. Could not fetch usable data from '%s'."), $post['address' . $x]);
					} else {
						array_push($address, ...$t_address);
					}
				} else {
					$data_errors[] = sprintf(gettext("URL '%s' is not valid."), $post['address' . $x]);
				}
				unset($t_address);
				rmdir_recursive($temp_filename);
			} elseif (is_alias($post['address' . $x]) &&
			    in_array(alias_get_type($post['address' . $x]), array('url', 'url_ports'))) {
				/* nested URL alias, see https://redmine.pfsense.org/issues/11863 */
				if (!$aliastable) {
					alias_make_table();
				}
				$t_address = explode(" ", $aliastable[$post['address' . $x]]);
				if ($t_address == null) {
					$data_errors[] = sprintf(gettext("A valid URL alias must be provided. Could not get usable data from '%s'."), $post['address' . $x]);
					continue;
				}
				$alias['aliasurl'][] = $post['address' . $x];
				if ($post["detail{$x}"] <> "") {
					if ((strpos($post["detail{$x}"], "||") === false) && (substr($post["detail{$x}"], 0, 1) != "|") && (substr($post["detail{$x}"], -1, 1) != "|")) {
						$final_address_details[] = $post["detail{$x}"];
					} else {
						/* Remove leading and trailing vertical bars and replace multiple vertical bars with single, */
						/* and put in the output array so the text is at least redisplayed for the user. */
						$final_address_details[] = preg_replace('/\|\|+/', '|', trim($post["detail{$x}"], "|"));
						if (!$desc_fmt_err_found) {
							$data_errors[] = $vertical_bar_err_text;
							$desc_fmt_err_found = true;
						}
					}
				} else {
					$final_address_details[] = sprintf(gettext("Entry added %s"), date('r'));
				}
				array_push($address, ...$t_address);
			} elseif ($post['address' . $x]) {
				$data_errors[] = sprintf(gettext("A valid URL or alias must be provided. Could not fetch usable data from '%s'."), $post['address' . $x]);
			}
		}
		unset($desc_fmt_err_found);
		if ($post['type'] == "url_ports") {
			$address = group_ports($address);
		}
	} else {
		/* item is a normal alias type */
		$wrongaliases = "";
		$desc_fmt_err_found = false;
		$alias_address_count = 0;
		$input_addresses = array();

		// First trim and expand the input data.
		// Users can paste strings like "10.1.2.0/24 10.3.0.0/16 9.10.11.0/24" into an address box.
		// They can also put an IP range.
		// This loop expands out that stuff so it can easily be validated.
		for ($x = 0; $x < ($max_alias_addresses - 1); $x++) {
			if ($post["address{$x}"] <> "") {
				if ($post["detail{$x}"] <> "") {
					if ((strpos($post["detail{$x}"], "||") === false) && (substr($post["detail{$x}"], 0, 1) != "|") && (substr($post["detail{$x}"], -1, 1) != "|")) {
						$detail_text = $post["detail{$x}"];
					} else {
						/* Remove leading and trailing vertical bars and replace multiple vertical bars with single, */
						/* and put in the output array so the text is at least redisplayed for the user. */
						$detail_text = preg_replace('/\|\|+/', '|', trim($post["detail{$x}"], "|"));
						if (!$desc_fmt_err_found) {
							$data_errors[] = $vertical_bar_err_text;
							$desc_fmt_err_found = true;
						}
					}
				} else {
					$detail_text = sprintf(gettext("Entry added %s"), date('r'));
				}

				$address_items = explode(" ", trim($post["address{$x}"]));
				foreach ($address_items as $address_item) {
					$address_item = alias_idn_to_ascii($address_item);
					$iprange_type = is_iprange($address_item);
					if ($iprange_type == 4) {
						list($startip, $endip) = explode('-', $address_item);
						if ($post['type'] == "network") {
							// For network type aliases, expand an IPv4 range into an array of subnets.
							$rangesubnets = ip_range_to_subnet_array($startip, $endip);
							foreach ($rangesubnets as $rangesubnet) {
								if ($alias_address_count > $max_alias_addresses) {
									break;
								}
								list($address_part, $subnet_part) = explode("/", $rangesubnet);
								$input_addresses[] = $address_part;
								$input_address_subnet[] = $subnet_part;
								$final_address_details[] = $detail_text;
								$alias_address_count++;
							}
						} else {
							// For host type aliases, expand an IPv4 range into a list of individual IPv4 addresses.
							$rangeaddresses = ip_range_to_address_array($startip, $endip, $max_alias_addresses - $alias_address_count);
							if (is_array($rangeaddresses)) {
								foreach ($rangeaddresses as $rangeaddress) {
									$input_addresses[] = $rangeaddress;
									$input_address_subnet[] = "";
									$final_address_details[] = $detail_text;
									$alias_address_count++;
								}
							} else {
								$data_errors[] = sprintf(gettext('Range is too large to expand into individual host IP addresses (%s)'), $address_item);
								$data_errors[] = sprintf(gettext('The maximum number of entries in an alias is %s'), $max_alias_addresses);
								// Put the user-entered data in the output anyway, so it will be re-displayed for correction.
								$input_addresses[] = $address_item;
								$input_address_subnet[] = "";
								$final_address_details[] = $detail_text;
							}
						}
					} else if ($iprange_type == 6) {
						$data_errors[] = sprintf(gettext('IPv6 address ranges are not supported (%s)'), $address_item);
						// Put the user-entered data in the output anyway, so it will be re-displayed for correction.
						$input_addresses[] = $address_item;
						$input_address_subnet[] = "";
						$final_address_details[] = $detail_text;
					} else {
						$subnet_type = is_subnet($address_item);
						if (($post['type'] == "host") && $subnet_type) {
							if ($subnet_type == 4) {
								// For host type aliases, if the user enters an IPv4 subnet, expand it into a list of individual IPv4 addresses.
								$subnet_size = subnet_size($address_item);
								if ($subnet_size > 0 &&
								    $subnet_size <= ($max_alias_addresses - $alias_address_count)) {
									$rangeaddresses = subnetv4_expand($address_item);
									foreach ($rangeaddresses as $rangeaddress) {
										$input_addresses[] = $rangeaddress;
										$input_address_subnet[] = "";
										$final_address_details[] = $detail_text;
										$alias_address_count++;
									}
								} else {
									$data_errors[] = sprintf(gettext('Subnet is too large to expand into individual host IP addresses (%s)'), $address_item);
									$data_errors[] = sprintf(gettext('The maximum number of entries in an alias is %s'), $max_alias_addresses);
									// Put the user-entered data in the output anyway, so it will be re-displayed for correction.
									$input_addresses[] = $address_item;
									$input_address_subnet[] = "";
									$final_address_details[] = $detail_text;
								}
							} else {
								$data_errors[] = sprintf(gettext('IPv6 subnets are not supported in host aliases (%s)'), $address_item);
								// Put the user-entered data in the output anyway, so it will be re-displayed for correction.
								$input_addresses[] = $address_item;
								$input_address_subnet[] = "";
								$final_address_details[] = $detail_text;
							}
						} else {
							list($address_part, $subnet_part) = explode("/", $address_item);
							if (!empty($subnet_part)) {
								if (is_subnet($address_item)) {
									$input_addresses[] = $address_part;
									$input_address_subnet[] = $subnet_part;
								} else {
									// The user typed something like "1.2.3.444/24" or "1.2.3.0/36" or similar rubbish.
									// Feed it through without splitting it apart, then it will be caught by the validation loop below.
									$input_addresses[] = $address_item;
									$input_address_subnet[] = "";
								}
							} else {
								$input_addresses[] = $address_part;
								$input_address_subnet[] = $post["address_subnet{$x}"];
							}
							$final_address_details[] = $detail_text;
							$alias_address_count++;
						}
					}
					if ($alias_address_count > $max_alias_addresses) {
						$data_errors[] = sprintf(gettext('The maximum number of entries in an alias has been exceeded (%s)'), $max_alias_addresses);
						break;
					}
				}
			}
		}

		// Validate the input data expanded above.
		foreach ($input_addresses as $idx => $input_address) {
			if (is_alias($input_address)) {
				if (!alias_same_type($input_address, $post['type'])) {
					/* alias type network can include alias type URL/URLTable.
					 * see https://redmine.pfsense.org/issues/1603
					 * and https://redmine.pfsense.org/issues/11863 */
					if (!($post['type'] == 'network' &&
					    preg_match("/url/i", alias_get_type($input_address)))) {
						$wrongaliases .= " " . $input_address;
					}
				}
			} else if ($post['type'] == "port") {
				if (!is_port_or_range($input_address)) {
					$data_errors[] = sprintf(gettext("%s is not a valid port or alias."), $input_address);
				}
			} else if (($post['type'] == "host") || ($post['type'] == "network")) {
				if (is_subnet($input_address) ||
				    (!is_ipaddr($input_address) && !is_hostname($input_address))) {
					$data_errors[] = sprintf(gettext('%1$s is not a valid %2$s address, FQDN or alias.'), $input_address, $singular_types[$post['type']]);
				}
			}
			$tmpaddress = $input_address;
			if (($post['type'] != "host") && is_ipaddr($input_address) && ($input_address_subnet[$idx] <> "")) {
				if (!is_subnet($input_address . "/" . $input_address_subnet[$idx])) {
					$data_errors[] = sprintf(gettext('%1$s/%2$s is not a valid subnet.'), $input_address, $input_address_subnet[$idx]);
				} else {
					$tmpaddress .= "/" . $input_address_subnet[$idx];
				}
			}
			$address[] = addrtolower($tmpaddress);
		}
		unset($desc_fmt_err_found);
		if ($wrongaliases <> "") {
			$data_errors[] = sprintf(gettext('The alias(es): %s cannot be nested because they are not of the same type.'), $wrongaliases);
		}
	}

	unset($vertical_bar_err_text);

	// Allow extending of the firewall edit page and include custom input validation
	pfSense_handle_custom_code("/usr/local/pkg/firewall_aliases_edit/input_validation");

	if (!$data_errors) {
		$alias['address'] = is_array($address) ? implode(" ", $address) : $address;
		$alias['descr'] = $post['descr'];
		$alias['type'] = $post['type'];
		$alias['detail'] = implode("||", $final_address_details);

		/*	 Check to see if alias name needs to be
		 *	 renamed on referenced rules and such
		 */
		if ($post['name'] <> $origname) {
			update_alias_name($post['name'], $origname);
		}

		pfSense_handle_custom_code("/usr/local/pkg/firewall_aliases_edit/pre_write_config");

		if (isset($id) && config_get_path('aliases/alias/' . $id)) {
			if (config_get_path('aliases/alias/' . $id . '/name') <> $alias['name']) {
				foreach (config_get_path('aliases/alias', []) as $aliasid => $aliasd) {
					if ($aliasd['address'] <> "") {
						$tmpdirty = false;
						$tmpaddr = explode(" ", $aliasd['address']);
						foreach ($tmpaddr as $tmpidx => $tmpalias) {
							if ($tmpalias == config_get_path('aliases/alias/' . $id . '/name')) {
								$tmpaddr[$tmpidx] = $alias['name'];
								$tmpdirty = true;
							}
						}
						if ($tmpdirty == true) {
							config_set_path('aliases/alias/' . $aliasid . '/address', implode(" ", $tmpaddr));
						}
					}
				}
			}
			config_set_path('aliases/alias/' . $id, $alias);
		} else {
			$alias_count = 0;
			while(config_get_path('aliases/alias/' . $alias_count)) {
				$alias_count++;
			}
			config_set_path('aliases/alias/' . $alias_count, $alias);
		}

		write_config(gettext("Edited a firewall alias."));

		/* restart related OpenVPN instances, see https://redmine.pfsense.org/issues/2668
		 * reload related static routes, see https://redmine.pfsense.org/issues/7547 */
		if (in_array($alias['type'], array('host', 'network', 'url'))) {
			$ovpnnets = array(
				'tunnel_network',
				'tunnel_networkv6',
				'local_network',
				'local_networkv6',
				'remote_network',
			       	'remote_networkv6');
			foreach (array("server", "client", "csc") as $type) {
				foreach (config_get_path('openvpn/openvpn-' . $type, []) as $settings) {
					if (!isset($settings['disable'])) {
						foreach ($ovpnnets as $nettype) {
							foreach (explode(',', $settings[$nettype]) as $net) {
								if ($net == $alias['name']) {
									if ($type == 'csc') {
										openvpn_resync_csc_all();
										break 4;
									} else {
										/* restart changed openvpn instance */
										openvpn_resync($type, $settings);
										break 2;
									}
								}
							}
						}
					}
				}
			}

			$srid = 0;
			foreach (config_get_path('staticroutes/route', []) as $sroute) {
				if (!isset($sroute['disabled']) &&
				    ($alias['name'] == $sroute['network'])) {
					delete_static_route($srid);
					$reload_static_route = true;
				}
				$srid++;
			}
			if ($reload_static_route && file_exists($g['tmp_path'] . '/.system_routes.apply')) {
				$toapplylist = unserialize_data(file_get_contents($g['tmp_path'] . '/.system_routes.apply'), []);
				foreach ($toapplylist as $toapply) {
					mwexec("{$toapply}");
				}
				@unlink($g['tmp_path'] . '/.system_routes.apply');

				$retval |= system_routing_configure();
				$retval |= filter_configure();
				/* reconfigure our gateway monitor */
				setup_gateways_monitor();

				if ($retval == 0) {
					clear_subsystem_dirty('staticroutes');
				}
			}
		}
	} else if (isset($pconfig)) {
		//we received input errors, copy data to prevent retype
		$pconfig['name'] = $post['name'];
		$pconfig['descr'] = $post['descr'];
		if (isset($alias['aliasurl']) && ($post['type'] == 'url') || ($post['type'] == 'url_ports')) {
			$pconfig['address'] = implode(" ", $alias['aliasurl']);
		} else {
			$pconfig['address'] = implode(" ", $address);
		}
		$pconfig['type'] = $post['type'];
		$pconfig['detail'] = implode("||", $final_address_details);
	}

	return $data_errors;
}

function do_local_input_validation($postdata, $reqdfields, $reqdfieldsn, &$input_errors) {

	/* check for bad control characters */
	foreach ($postdata as $pn => $pd) {
		if (is_string($pd) && preg_match("/[\\x00-\\x08\\x0b\\x0c\\x0e-\\x1f]/", $pd)) {
			$input_errors[] = sprintf(gettext("The field %s contains invalid characters."), $pn);
		}
	}

	if (is_array($reqdfields)) {
		for ($i = 0; $i < count($reqdfields); $i++) {
			if ($postdata[$reqdfields[$i]] == "") {
				$input_errors[] = sprintf(gettext("The field %s is required."), $reqdfieldsn[$i]);
			}
		}
	}
}

?>
