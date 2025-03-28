<?php
/*
 * system_crlmanager.php
 *
 * part of pfSense (https://www.pfsense.org)
 * Copyright (c) 2004-2013 BSD Perimeter
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

##|+PRIV
##|*IDENT=page-system-crlmanager
##|*NAME=System: CRL Manager
##|*DESCR=Allow access to the 'System: CRL Manager' page.
##|*MATCH=system_crlmanager.php*
##|-PRIV

require_once("guiconfig.inc");
require_once("certs.inc");
require_once("openvpn.inc");
require_once("pfsense-utils.inc");
require_once("vpn.inc");

$max_lifetime = crl_get_max_lifetime();
$default_lifetime = min(730, $max_lifetime);

global $openssl_crl_status;

$crl_methods = array(
	"internal" => gettext("Create an internal Certificate Revocation List"),
	"existing" => gettext("Import an existing Certificate Revocation List"));

if (isset($_REQUEST['id']) && ctype_alnum($_REQUEST['id'])) {
	$id = $_REQUEST['id'];
}


/* Clean up blank entries missing a reference ID */
foreach (config_get_path('crl', []) as $cid => $acrl) {
	if (!isset($acrl['refid'])) {
		config_del_path("crl/{$cid}");
	}
}

$act = $_REQUEST['act'];

$cacert_list = array();

if (!empty($id)) {
	$crl_item_config = lookup_crl($id);
	$thiscrl = &$crl_item_config['item'];
}

/* Actions other than 'new' require a CRL to act upon.
 * 'del' action must be submitted via POST. */
if ((!empty($act) &&
    ($act != 'new') &&
    !$thiscrl) ||
    (($act == 'del') && empty($_POST))) {
	pfSenseHeader("system_camanager.php");
	$act="";
	$savemsg = gettext("Invalid CRL reference.");
	$class = "danger";
}

switch ($act) {
	case 'del':
		$name = htmlspecialchars($thiscrl['descr']);
		if (crl_in_use($id)) {
			$savemsg = sprintf(gettext("Certificate Revocation List %s is in use and cannot be deleted."), $name);
			$class = "danger";
		} else {
			foreach (config_get_path('crl', []) as $cid => $acrl) {
				if ($acrl['refid'] == $thiscrl['refid']) {
					config_del_path("crl/{$cid}");
				}
			}
			write_config("Deleted CRL {$name}.");
			$savemsg = sprintf(gettext("Certificate Revocation List %s successfully deleted."), $name);
			$class = "success";
		}
		break;
	case 'new':
		$pconfig['method'] = $_REQUEST['method'];
		$pconfig['caref'] = $_REQUEST['caref'];
		$pconfig['lifetime'] = $default_lifetime;
		$pconfig['serial'] = "0";
		$crlca = lookup_ca($pconfig['caref']);
		$crlca = $crlca['item'];
		if (!$crlca) {
			$input_errors[] = gettext('Invalid CA');
			unset($act);
		}
		break;
	case 'addcert':
		unset($input_errors);
		$pconfig = $_REQUEST;

		/* input validation */
		$reqdfields = explode(" ", "descr id");
		$reqdfieldsn = array(
			gettext("Descriptive name"),
			gettext("CRL ID"));

		do_input_validation($_POST, $reqdfields, $reqdfieldsn, $input_errors);

		if (preg_match("/[\?\>\<\&\/\\\"\']/", $pconfig['descr'])) {
			array_push($input_errors, "The field 'Descriptive Name' contains invalid characters.");
		}
		if ($pconfig['lifetime'] > $max_lifetime) {
			$input_errors[] = gettext("Lifetime is longer than the maximum allowed value. Use a shorter lifetime.");
		}
		if ((strlen($pconfig['serial']) > 0) && !cert_validate_serial($pconfig['serial'])) {
			$input_errors[] = gettext("Please enter a valid integer serial number.");
		}

		$revoke_list = array();
		if (!$pconfig['crlref']) {
			pfSenseHeader("system_crlmanager.php");
			exit;
		}
		$crl_item_config = lookup_crl($pconfig['crlref']);
		$crl = &$crl_item_config['item'];

		if (!is_array($pconfig['certref'])) {
			$pconfig['certref'] = array();
		}
		if (!is_crl_internal($crl)) {
			$input_errors[] = gettext("Cannot revoke certificates for an imported/external CRL.");
		}
		if (!empty($pconfig['revokeserial'])) {
			foreach (explode(' ', $pconfig['revokeserial']) as $serial) {
				$vserial = cert_validate_serial($serial, true, true);
				if ($vserial != null) {
					$revoke_list[] = $vserial;
				} else {
					$input_errors[] = gettext("Invalid serial in list (Must be ASN.1 integer compatible decimal or hex string).");
				}
			}
		}
		if (empty($pconfig['save']) && empty($pconfig['certref']) && empty($revoke_list)) {
			$input_errors[] = gettext("Select one or more certificates or enter a serial number to revoke.");
		}
		foreach ($pconfig['certref'] as $rcert) {
			$cert = lookup_cert($rcert);
			$cert = $cert['item'];
			if ($crl['caref'] == $cert['caref']) {
				$revoke_list[] = $cert;
			} else {
				$input_errors[] = gettext("CA mismatch between the Certificate and CRL. Unable to Revoke.");
			}
		}

		if (!$input_errors) {
			$crl['descr'] = $pconfig['descr'];
			$crl['lifetime'] = $pconfig['lifetime'];
			$crl['serial'] = $pconfig['serial'];
			if (!empty($revoke_list)) {
				$savemsg = "Revoked certificate(s) in CRL {$crl['descr']}.";
				$reason = (empty($pconfig['crlreason'])) ? 0 : $pconfig['crlreason'];
				foreach ($revoke_list as $cert) {
					cert_revoke($cert, $crl_item_config, $reason);
				}
				// refresh IPsec and OpenVPN CRLs
				openvpn_refresh_crls();
				ipsec_configure();
			} else {
				$savemsg = "Saved CRL {$crl['descr']}.";
			}
			write_config($savemsg);
			pfSenseHeader("system_crlmanager.php");
			exit;
		} else {
			$act = 'edit';
		}
		break;
	case 'delcert':
		if (!is_array($thiscrl['cert'])) {
			pfSenseHeader("system_crlmanager.php");
			exit;
		}
		$found = false;
		foreach ($thiscrl['cert'] as $acert) {
			if ($acert['refid'] == $_REQUEST['certref']) {
				$found = true;
				$thiscert = $acert;
			}
		}
		if (!$found) {
			pfSenseHeader("system_crlmanager.php");
			exit;
		}
		$certname = htmlspecialchars($thiscert['descr']);
		$crlname = htmlspecialchars($thiscrl['descr']);
		if (cert_unrevoke($thiscert, $crl_item_config)) {
			$savemsg = sprintf(gettext('Deleted Certificate %1$s from CRL %2$s.'), $certname, $crlname);
			$class = "success";
			// refresh IPsec and OpenVPN CRLs
			openvpn_refresh_crls();
			ipsec_configure();
			write_config($savemsg);
		} else {
			$savemsg = sprintf(gettext('Failed to delete Certificate %1$s from CRL %2$s.'), $certname, $crlname);
			$class = "danger";
		}
		$act="edit";
		break;
	case 'exp':
		/* Exporting the CRL contents*/
		crl_update($crl_item_config);
		send_user_download('data', base64_decode($thiscrl['text']), "{$thiscrl['descr']}.crl");
		break;
	default:
		break;
}

if ($_POST['save'] && empty($input_errors)) {
	$input_errors = array();
	$pconfig = $_POST;

	/* input validation */
	if (($pconfig['method'] == "existing") || ($act == "editimported")) {
		$reqdfields = explode(" ", "descr crltext");
		$reqdfieldsn = array(
			gettext("Descriptive name"),
			gettext("Certificate Revocation List data"));
	}
	if (($pconfig['method'] == "internal") ||
	    ($act == "addcert")) {
		$reqdfields = explode(" ", "descr caref");
		$reqdfieldsn = array(
			gettext("Descriptive name"),
			gettext("Certificate Authority"));
	}

	do_input_validation($_POST, $reqdfields, $reqdfieldsn, $input_errors);

	if (preg_match("/[\?\>\<\&\/\\\"\']/", $pconfig['descr'])) {
		array_push($input_errors, "The field 'Descriptive Name' contains invalid characters.");
	}
	if ($pconfig['lifetime'] > $max_lifetime) {
		$input_errors[] = gettext("Lifetime is longer than the maximum allowed value. Use a shorter lifetime.");
	}

	if ((strlen($pconfig['serial']) > 0) && !cert_validate_serial($pconfig['serial'])) {
		$input_errors[] = gettext("Please enter a valid integer serial number.");
	}

	/* save modifications */
	if (!$input_errors) {
		$result = false;

		if ($thiscrl) {
			$crl =& $thiscrl;
		} else {
			$crl = array();
			$crl['refid'] = uniqid();
		}

		$crl['descr'] = $pconfig['descr'];
		if ($act != "editimported") {
			$crl['caref'] = $pconfig['caref'];
			$crl['method'] = $pconfig['method'];
		}

		if (($pconfig['method'] == "existing") || ($act == "editimported")) {
			$crl['text'] = base64_encode($pconfig['crltext']);
		}

		if ($pconfig['method'] == "internal") {
			$crl['serial'] = empty($pconfig['serial']) ? '0' : $pconfig['serial'];
			$crl['lifetime'] = empty($pconfig['lifetime']) ? $default_lifetime : $pconfig['lifetime'];
			$crl['cert'] = array();
		}

		if (!$thiscrl) {
			config_set_path('crl/', $crl);
		} else {
			config_set_path("crl/{$crl_item_config['idx']}", $crl);
		}

		write_config("Saved CRL {$crl['descr']}");
		// refresh IPsec and OpenVPN CRLs
		openvpn_refresh_crls();
		ipsec_configure();
		pfSenseHeader("system_crlmanager.php");
	}
}

$pgtitle = array(gettext('System'), gettext('Certificates'), gettext('Revocation'));
$pglinks = array("", "system_camanager.php", "system_crlmanager.php");

if ($act == "new" || $act == gettext("Save") || $input_errors || $act == "edit") {
	$pgtitle[] = gettext('Edit');
	$pglinks[] = "@self";
}
include("head.inc");
?>

<script type="text/javascript">
//<![CDATA[

function method_change() {

	method = document.iform.method.value;

	switch (method) {
		case "internal":
			document.getElementById("existing").style.display="none";
			document.getElementById("internal").style.display="";
			break;
		case "existing":
			document.getElementById("existing").style.display="";
			document.getElementById("internal").style.display="none";
			break;
	}
}

//]]>
</script>

<?php

function build_method_list($importonly = false) {
	global $_POST, $crl_methods;

	$list = array();

	foreach ($crl_methods as $method => $desc) {
		if ($importonly && ($method != "existing")) {
			continue;
		}

		$list[$method] = $desc;
	}

	return($list);
}

function build_ca_list() {
	$list = array();

	foreach (config_get_path('ca', []) as $ca) {
		$list[$ca['refid']] = $ca['descr'];
	}

	return($list);
}

function build_cacert_list() {
	global $crl, $id;

	$list = array();
	foreach (config_get_path('cert', []) as $cert) {
		if ((isset($cert['caref']) && !empty($cert['caref'])) &&
		    ($cert['caref'] == $crl['caref']) &&
		    !is_cert_revoked($cert, $id)) {
			$list[$cert['refid']] = $cert['descr'];
		}
	}

	return($list);
}

if ($input_errors) {
	print_input_errors($input_errors);
}

if ($savemsg) {
	print_info_box($savemsg, $class);
}

$tab_array = array();
$tab_array[] = array(gettext('Authorities'), false, 'system_camanager.php');
$tab_array[] = array(gettext('Certificates'), false, 'system_certmanager.php');
$tab_array[] = array(gettext('Revocation'), true, 'system_crlmanager.php');
display_top_tabs($tab_array);

if ($act == "new" || $act == gettext("Save")) {
	$form = new Form();

	$section = new Form_Section('Create new Revocation List');

	$section->addInput(new Form_StaticText(
		'Certificate Authority',
		$crlca['descr']
	));

	if (!isset($id)) {
		$section->addInput(new Form_Select(
			'method',
			'*Method',
			$pconfig['method'],
			build_method_list((!isset($crlca['prv']) || empty($crlca['prv'])))
		));
	}

	$section->addInput(new Form_Input(
		'descr',
		'*Descriptive name',
		'text',
		$pconfig['descr']
	));

	$form->addGlobal(new Form_Input(
		'caref',
		null,
		'hidden',
		$pconfig['caref']
	));

	$form->add($section);

	$section = new Form_Section('Existing Certificate Revocation List');
	$section->addClass('existing');

	$section->addInput(new Form_Textarea(
		'crltext',
		'*CRL data',
		$pconfig['crltext']
		))->setHelp('Paste a Certificate Revocation List in X.509 CRL format here.');

	$form->add($section);

	$section = new Form_Section('Internal Certificate Revocation List');
	$section->addClass('internal');

	$section->addInput(new Form_Input(
		'lifetime',
		'Lifetime (Days)',
		'number',
		$pconfig['lifetime'],
		['max' => $max_lifetime]
	));

	$section->addInput(new Form_Input(
		'serial',
		'Serial',
		'number',
		$pconfig['serial'],
		['min' => '0']
	));

	$form->add($section);

	if (isset($id) && $thiscrl) {
		$form->addGlobal(new Form_Input(
			'id',
			null,
			'hidden',
			$id
		));
	}

	print($form);

} elseif ($act == "editimported") {

	$form = new Form();

	$section = new Form_Section('Edit Imported Certificate Revocation List');

	$section->addInput(new Form_Input(
		'descr',
		'*Descriptive name',
		'text',
		$thiscrl['descr']
	));

	$section->addInput(new Form_Textarea(
		'crltext',
		'*CRL data',
		!empty($thiscrl['text']) ? base64_decode($thiscrl['text']) : ''
	))->setHelp('Paste a Certificate Revocation List in X.509 CRL format here.');

	$form->addGlobal(new Form_Input(
		'id',
		null,
		'hidden',
		$id
	));

	$form->addGlobal(new Form_Input(
		'act',
		null,
		'hidden',
		'editimported'
	));

	$form->add($section);

	print($form);

} elseif ($act == "edit") {
	$crl = $thiscrl;

	$form = new Form();

	$section = new Form_Section('Edit Internal Certificate Revocation List');

	$section->addInput(new Form_Input(
		'descr',
		'*Descriptive name',
		'text',
		$crl['descr']
	));

	$section->addInput(new Form_Input(
		'lifetime',
		'CRL Lifetime (Days)',
		'number',
		$crl['lifetime'],
		['max' => $max_lifetime]
	));

	$section->addInput(new Form_Input(
		'serial',
		'CRL Serial',
		'number',
		$crl['serial'],
		['min' => '0']
	));

	$form->add($section);
?>

	<div class="panel panel-default">
		<div class="panel-heading"><h2 class="panel-title"><?=gettext("Revoked Certificates in CRL") . ': ' . $crl['descr']?></h2></div>
		<div class="panel-body table-responsive">
<?php
	if (!is_array($crl['cert']) || (count($crl['cert']) == 0)) {
		print_info_box(gettext("No certificates found for this CRL."), 'danger');
	} else {
?>
			<table class="table table-striped table-hover table-condensed sortable-theme-bootstrap" data-sortable>
				<thead>
					<tr>
						<th><?=gettext("Serial")?></th>
						<th><?=gettext("Certificate Name")?></th>
						<th><?=gettext("Revocation Reason")?></th>
						<th><?=gettext("Revoked At")?></th>
						<th></th>
					</tr>
				</thead>
				<tbody>
<?php
		foreach ($crl['cert'] as $cert):
			$name = empty($cert['descr']) ? gettext('Revoked by Serial') : htmlspecialchars($cert['descr']);
			$serial = crl_get_entry_serial($cert);
			if (strlen($serial) == 0) {
				$serial = gettext("Invalid");
			} ?>
					<tr>
						<td><?=htmlspecialchars($serial);?></td>
						<td><?=$name; ?></td>
						<td><?=$openssl_crl_status[$cert['reason']]; ?></td>
						<td><?=date("D M j G:i:s T Y", $cert['revoke_time']); ?></td>
						<td class="list">
							<a href="system_crlmanager.php?act=delcert&amp;id=<?=$crl['refid']; ?>&amp;certref=<?=$cert['refid']; ?>" usepost>
								<i class="fa-solid fa-trash-can" title="<?=gettext("Delete this certificate from the CRL")?>" alt="<?=gettext("Delete this certificate from the CRL")?>"></i>
							</a>
						</td>
					</tr>
<?php
		endforeach;
?>
				</tbody>
			</table>
<?php
	}
?>
		</div>
	</div>
<?php

	$section = new Form_Section('Revoke Certificates');

	$section->addInput(new Form_Select(
		'crlreason',
		'Reason',
		-1,
		$openssl_crl_status
		))->setHelp('Select the reason for which the certificates are being revoked.');

	$cacert_list = build_cacert_list();
	if (count($cacert_list) == 0) {
		print_info_box(gettext("No certificates found for this CA."), 'danger');
	} else {
		$section->addInput(new Form_Select(
			'certref',
			'Revoke Certificates',
			$pconfig['certref'],
			$cacert_list,
			true
			))->addClass('multiselect')
			->setHelp('Hold down CTRL (PC)/COMMAND (Mac) key to select multiple items.');
	}

	$section->addInput(new Form_Input(
		'revokeserial',
		'Revoke by Serial',
		'text',
		$pconfig['revokeserial']
	))->setHelp('List of certificate serial numbers to revoke (separated by spaces)');

	$form->addGlobal(new Form_Button(
		'submit',
		'Add',
		null,
		'fa-solid fa-plus'
		))->addClass('btn-success btn-sm');

	$form->addGlobal(new Form_Input(
		'id',
		null,
		'hidden',
		$crl['refid']
	));

	$form->addGlobal(new Form_Input(
		'act',
		null,
		'hidden',
		'addcert'
	));

	$form->addGlobal(new Form_Input(
		'crlref',
		null,
		'hidden',
		$crl['refid']
	));

	$form->add($section);

	print($form);
} else {
?>

	<div class="panel panel-default">
		<div class="panel-heading"><h2 class="panel-title"><?=gettext("Certificate Revocation Lists")?></h2></div>
		<div class="panel-body table-responsive">
			<table class="table table-striped table-hover table-condensed table-rowdblclickedit">
				<thead>
					<tr>
						<th><?=gettext("CA")?></th>
						<th><?=gettext("Name")?></th>
						<th><?=gettext("Internal")?></th>
						<th><?=gettext("Certificates")?></th>
						<th><?=gettext("In Use")?></th>
						<th><?=gettext("Actions")?></th>
					</tr>
				</thead>
				<tbody>
<?php
	$pluginparams = array();
	$pluginparams['type'] = 'certificates';
	$pluginparams['event'] = 'used_crl';
	$certificates_used_by_packages = pkg_call_plugins('plugin_certificates', $pluginparams);
	// Map CRLs to CAs in one pass
	$ca_crl_map = array();
	foreach (config_get_path('crl', []) as $crl) {
		$ca_crl_map[$crl['caref']][] = $crl['refid'];
	}

	$i = 0;
	foreach (config_get_path('ca', []) as $ca):
		$caname = htmlspecialchars($ca['descr']);
		if (is_array($ca_crl_map[$ca['refid']])):
			foreach ($ca_crl_map[$ca['refid']] as $crl):
				$tmpcrl = lookup_crl($crl);
				$tmpcrl = $tmpcrl['item'];
				$internal = is_crl_internal($tmpcrl);
				if ($internal && (!isset($tmpcrl['cert']) || empty($tmpcrl['cert'])) ) {
					$tmpcrl['cert'] = array();
				}
				$inuse = crl_in_use($tmpcrl['refid']);
?>
					<tr>
						<td><?=$caname?></td>
						<td><?=$tmpcrl['descr']; ?></td>
						<td><i class="<?=($internal) ? "fa-solid fa-check" : "fa-solid fa-times"; ?>"></i></td>
						<td><?=($internal) ? count($tmpcrl['cert']) : "Unknown (imported)"; ?></td>
						<td>
						<?php if (is_openvpn_server_crl($tmpcrl['refid'])): ?>
							<?=gettext("OpenVPN Server")?>
						<?php endif?>
						<?php echo cert_usedby_description($tmpcrl['refid'], $certificates_used_by_packages); ?>
						</td>
						<td>
							<a href="system_crlmanager.php?act=exp&amp;id=<?=$tmpcrl['refid']?>" class="fa-solid fa-download" title="<?=gettext("Export CRL")?>" ></a>
<?php
				if ($internal): ?>
							<a href="system_crlmanager.php?act=edit&amp;id=<?=$tmpcrl['refid']?>" class="fa-solid fa-pencil" title="<?=gettext("Edit CRL")?>"></a>
<?php
				else:
?>
							<a href="system_crlmanager.php?act=editimported&amp;id=<?=$tmpcrl['refid']?>" class="fa-solid fa-pencil" title="<?=gettext("Edit CRL")?>"></a>
<?php			endif;
				if (!$inuse):
?>
							<a href="system_crlmanager.php?act=del&amp;id=<?=$tmpcrl['refid']?>" class="fa-solid fa-trash-can" title="<?=gettext("Delete CRL")?>" usepost></a>
<?php
				endif;
?>
						</td>
					</tr>
<?php
				$i++;
				endforeach;
			endif;
			$i++;
		endforeach;
?>
				</tbody>
			</table>
		</div>
	</div>

<?php
	$form = new Form(false);
	$section = new Form_Section('Create or Import a New Certificate Revocation List');
	$group = new Form_Group(null);
	$group->add(new Form_Select(
		'caref',
		'Certificate Authority',
		null,
		build_ca_list()
		))->setHelp('Select a Certificate Authority for the new CRL');
	$group->add(new Form_Button(
		'submit',
		'Add',
		null,
		'fa-solid fa-plus'
		))->addClass('btn-success btn-sm');
	$section->add($group);
	$form->addGlobal(new Form_Input(
		'act',
		null,
		'hidden',
		'new'
	));
	$form->add($section);
	print($form);
}

?>

<script type="text/javascript">
//<![CDATA[
events.push(function() {

	// Hides all elements of the specified class. This will usually be a section or group
	function hideClass(s_class, hide) {
		if (hide) {
			$('.' + s_class).hide();
		} else {
			$('.' + s_class).show();
		}
	}

	// When the 'method" selector is changed, we show/hide certain sections
	$('#method').on('change', function() {
		hideClass('internal', ($('#method').val() == 'existing'));
		hideClass('existing', ($('#method').val() == 'internal'));
	});

	hideClass('internal', ($('#method').val() == 'existing'));
	hideClass('existing', ($('#method').val() == 'internal'));
	$('.multiselect').attr("size","<?= max(3, min(15, count($cacert_list))) ?>");
});
//]]>
</script>

<?php include("foot.inc");
