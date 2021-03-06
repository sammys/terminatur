<?php
/**
 * @file
 * Initialization functionality and Pantheon API integration.
 */

// Constants
define('TERMINATUR_SSH_KEY', 'terminatur.id_rsa');
define('TERMINATUR_PANTHEON_ALIASES_FILE', TERMINATUR_DRUSH_DIR . 'pantheon.aliases.drushrc.php');
define('TERMINATUR_TERMINATUR_ALIASES_FILE', TERMINATUR_DRUSH_DIR . 'terminatur.aliases.drushrc.php');

/**
 * Ensures user has set up SSH and has downloaded their Pantheon aliases file.
 *
 * If either isn't set up, it authenticates with Pantheon and carries out the
 * necessary setup steps.
 */
function terminatur_bootstrap($force_refresh = FALSE) {
  // Static cache the aliases
  static $aliases;

  if (!isset($aliases)) {
    // Handle all the key things
    _terminatur_keys();
    // Download and parse the aliases file if it's not there.
    // @todo: might want to do some sort of validation here first?
    $aliases = array();
    if (!file_exists(TERMINATUR_TERMINATUR_ALIASES_FILE) || $force_refresh === TRUE) {
      _terminatur_download_aliases();
      $aliases = _terminatur_parse_aliases();
    }
    else {
      require TERMINATUR_TERMINATUR_ALIASES_FILE;
    }
  }

  // @todo Validate the pantheon aliases file.
  return $aliases;
}

/**
 * Returns data for the current Terminus session, authenticating if necessary.
 *
 * @return array
 *   Terminus session data.
 */
function _terminatur_get_session() {
  // Use current session if there is one.
  $session_data = terminus_bootstrap();
  if (isset($session_data['email']) && terminus_validate_session($session_data)) {
    return $session_data;
  }
  // Otherwise, authenticate with Terminus.
  // @todo: we need a way to do this that is not interactive for kalaboxage
  $email = drush_prompt(dt('Pantheon dashboard email'), NULL, TRUE);
  drush_terminus_pantheon_auth($email);
  // @todo Handle authentication failure.
  return terminus_bootstrap();
}

/**
 * Returns data for the current Terminus session, authenticating if necessary.
 *
 * @return array
 *   Terminus session data.
 */
function _terminatur_keys() {
  // Load the Terminatur-generated ssh key if there's one.
  _terminatur_load_default_key();
  // Make sure we have a key that'll work with Pantheon.
  $pantheon_keys = drush_cache_get('pantheon-keys-gotcha', 'terminatur');
  if (!$pantheon_keys) {
    // Check for a SSH key that works with Pantheon.
    $pantheon_keys = _terminatur_get_pantheon_keys();
    // If user doesn't have a SSH key set up with Pantheon, offer to make one for them.
    if (!$pantheon_keys) {
      $pantheon_keys = array(_terminatur_set_up_key());
      _terminatur_load_default_key();
    }
    drush_cache_set('pantheon-keys-gotcha', $pantheon_keys, 'terminatur');
  }
  else {
    // If we have a cached key(s), make sure it's loaded.
    $key_loaded = FALSE;
    foreach ($pantheon_keys->data as $key) {
      if (_terminatur_is_key_loaded($key)) {
        $key_loaded = TRUE;
        break;
      }
    }
    if (!$key_loaded) {
      // If the key isn't loaded, remove it from the cache and start over.
      drush_cache_clear_all('pantheon-keys-gotcha', 'terminatur');
      _terminatur_keys();
    }
  }
}

/**
 * Get's
 *
 * @return array
 *   Terminus session data.
 */
function _terminatur_get_user() {
  // Use current session if there is one.
  $session_data = terminus_bootstrap();
  if (isset($session_data['email']) && terminus_validate_session($session_data)) {
    $session = $session_data;
  }
  else {
    // Otherwise, authenticate with Terminus.
    // @todo: we need a way to do this that is not interactive for kalaboxage
    $email = drush_prompt(dt('Pantheon dashboard email'), NULL, TRUE);
    drush_terminus_pantheon_auth($email);
    // @todo Handle authentication failure.
    $session = terminus_bootstrap();
  }

  $result = terminus_api_user_get($session['user_uuid']);
  return json_decode($result['json']);
}

/**
 * Finds any keys on the host that are registered with Pantheon.
 *
 * @return array|bool
 *   An array of public keys registered with Pantheon, or false if there are none.
 */
function _terminatur_get_pantheon_keys() {
  $session_data = _terminatur_get_session();
  $user_id = $session_data['user_uuid'];
  // Get list of keys on Pantheon and keys on host.
  $pantheon_keys = terminus_api_user_sshkey_get($user_id);
  $pantheon_keys = array_values(drush_json_decode($pantheon_keys['json']));
  drush_shell_exec('ssh-add -L');
  $host_keys = drush_shell_exec_output();
  $pantheon_keys = array_map('_terminatur_extract_key', $pantheon_keys);
  $host_keys = array_map('_terminatur_extract_key', $host_keys);
  $intersection = array_intersect($host_keys, $pantheon_keys);
  if (!empty($intersection)) {
    return $intersection;
  }
  return FALSE;
}

/**
 * Extracts a public SSH key's body.
 *
 * @param string $key_data
 *   Data from the public key file.
 *
 * @return string|bool
 *   Key body or false if unable to extract it.
 */
function _terminatur_extract_key($key_data) {
  $matches = array();
  preg_match('/^ssh-(?:rsa|dsa)\s([^\s]+)\s.+$/', $key_data, $matches);
  if ($matches && isset($matches[1])){
    return $matches[1];
  }
  return FALSE;
}

/**
 * Generates a SSH key and adds it to the user's Pantheon account.
 *
 * @return string
 *   The generated public key.
 */
function _terminatur_set_up_key() {
  // @todo Ask permission via cli.
  $session_data = _terminatur_get_session();
  $user_id = $session_data['user_uuid'];
  // Generate a key on the host.
  // @todo Remove existing Terminatur ssh key if it exists.
  drush_shell_exec('hostname -f');
  $hostname = drush_shell_exec_output();
  $key_file = getenv('HOME') . '/.ssh/' . TERMINATUR_SSH_KEY;
  $interact = file_exists($key_file) ? " -y" : "";
  drush_shell_exec('ssh-keygen -C "terminatur@' . $hostname[0]  . '" -f "' . $key_file . '" -P ""' . $interact);
  // Read generated key in and submit it to Terminus.
  $key = file_get_contents($key_file . '.pub');
  terminus_api_sshkey_validate($user_id, $key);
  // @todo Check key validation result.
  return $key;
}

/**
 * Checks if a given ssh key is loaded into the agent.
 *
 * @param string $key
 *   The body of the key to check for.
 *
 * @return bool
 *   TRUE if loaded, FALSE if not.
 */
function _terminatur_is_key_loaded($key) {
  drush_shell_exec('ssh-add -L');
  $host_keys = drush_shell_exec_output();
  $host_keys = array_map('_terminatur_extract_key', $host_keys);
  return in_array(_terminatur_extract_key($key), $host_keys);
}

/**
 * Loads the Terminatur-generated ssh key if it exists.
 */
function _terminatur_load_default_key() {
  if (file_exists(getenv('HOME') . '/.ssh/' . TERMINATUR_SSH_KEY)) {
    drush_shell_exec('eval "$(ssh-agent)"');
    drush_shell_exec('ssh-add ' . getenv('HOME') . '/.ssh/' . TERMINATUR_SSH_KEY);
    return TRUE;
  }
  return FALSE;
}

/**
 * Calls Terminus to download the user's aliases file to /tmp.
 */
function _terminatur_download_aliases() {
  $session_data = _terminatur_get_session();
  drush_set_option('destination', TERMINATUR_PANTHEON_ALIASES_FILE);
  drush_terminus_pantheon_aliases_validate();
  drush_terminus_pantheon_aliases();
}

/**
 * Parse Pantheon alias file for use with terminatur
 */
function _terminatur_parse_aliases() {
  // Load in an unblemished Pantheon alias file
  $aliases = array();
  if (file_exists(TERMINATUR_PANTHEON_ALIASES_FILE)) {
    require TERMINATUR_PANTHEON_ALIASES_FILE;
  }
  else {
    return FALSE;
  }

  // Strip out uneeded enviornments
  _terminatur_strip_aliases($aliases);

  // Add stuff
  _terminatur_aliases_options($aliases);
  _terminatur_code_options($aliases);
  _terminatur_data_options($aliases);
  _terminatur_file_options($aliases);

  // Write to file
  _terminatur_write_aliases($aliases);

  // Return
  return $aliases;
}

/**
 * Remove uneeded environments
 */
function _terminatur_strip_aliases(&$sites) {
  // Remove all non-dev environments
  // @todo: eventually we want this to be all dev and multisite environments
  // if (strpos($key, '.test') !== FALSE || strpos($key, '.live') !== FALSE) {
  foreach ($sites as $key => $value) {
    if (strpos($key, '.dev') === FALSE) {
      unset($sites[$key]);
    }
  }
}

/**
 * Remove uneeded environments
 */
function _terminatur_aliases_options(&$sites) {
  // Parse remaining aliases so they can be Terminatud
  foreach ($sites as $key => $value) {
    // Get sitename and environment
    $site_parts = explode('.', $key);
    $sites[$key]['machine-name'] = $site_parts[0];
    $sites[$key]['env'] = $site_parts[1];

    // Connect via terminus to get other fun stuff
    // If there is a preexisting terminuatur alias file let's load the site
    // uuid from there to save a request
    $aliases = array();
    if (file_exists(TERMINATUR_TERMINATUR_ALIASES_FILE)) {
      require TERMINATUR_TERMINATUR_ALIASES_FILE;
      $sites[$key]['uuid'] = $aliases[$key]['uuid'];
    }
    if (empty($sites[$key]['uuid'])) {
      $sites[$key]['uuid'] = terminus_get_site_uuid_by_name($sites[$key]['machine-name']);
    }

    // Build other relevant things
    $sites[$key]['ssh-options'] .= ' -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no';
    $sites[$key]['built-from'] = $key;
  }
}

/**
 * Get options for code
 */
function _terminatur_code_options(&$aliases) {
  // Check if user has git
  $method = drush_shell_exec('git --version') ? 'git' : 'download';
  foreach ($aliases as $key => $value) {
    $aliases[$key]['terminatur']['code']['method'] = $method;
    switch ($method) {
      case 'git':
        $aliases[$key]['terminatur']['code']['git-connect'] = "ssh://codeserver." . $aliases[$key]['env'] . "." . $aliases[$key]['uuid'] . "@codeserver." . $aliases[$key]['env'] . "." . $aliases[$key]['uuid'] . ".drush.in:2222/~/repository.git";
        break;
      case 'download':
        $bucket = terminus_latest_bucket($aliases[$key]['uuid'], $aliases[$key]['env'], 'code');
        $aliases[$key]['terminatur']['code']['download-bucket'] = $bucket ? $bucket : 'NONE';
        break;
    }
  }
}

/**
 * Get options for data
 */
function _terminatur_data_options(&$aliases) {
  // Check if user has MySQL
  $method = drush_shell_exec('mysql --version') ? 'mysql' : 'download';
  foreach ($aliases as $key => $value) {
    // Build DB stuff
    $regex = '/^mysql:\/\/([^:]+):([^@]+)@([^:]+):(\d+)\/(.+)$/';
    $matches = array();
    preg_match($regex, $aliases[$key]['db-url'], $matches);
    $aliases[$key]['terminatur']['database']['db-user'] = $matches[1];
    $aliases[$key]['terminatur']['database']['db-pass'] = $matches[2];
    $aliases[$key]['terminatur']['database']['db-host'] = $matches[3];
    $aliases[$key]['terminatur']['database']['db-port'] = $matches[4];
    $aliases[$key]['terminatur']['database']['db-db'] = $matches[5];
    $aliases[$key]['terminatur']['database']['local-db'] = str_replace("-", "_", $aliases[$key]['machine-name'] . TERMINATUR_DB_SUFFIX);
    $aliases[$key]['terminatur']['database']['method'] = $method;

    // This request doesn't make any sense
    // $bucket = terminus_latest_bucket($aliases[$key]['uuid'], $aliases[$key]['env'], 'database');
    $bucket = FALSE;
    $aliases[$key]['terminatur']['database']['download-bucket'] = $bucket ? $bucket : 'NONE';

  }
}

/**
 * Get options for files
 */
function _terminatur_file_options(&$aliases) {
    // Check if user has rsync
  $method = drush_shell_exec('rsync --version') ? 'rsync' : 'download';
  foreach ($aliases as $key => $value) {
    $aliases[$key]['terminatur']['files']['method'] = $method;
    switch ($method) {
      case 'rsync':
        $rsync = array(
          'options' => '-rlztv --ipv4',
          'ssh' => addcslashes("-e 'ssh " . $aliases[$key]['ssh-options'] . "'", "'"),
          'excludes' => addcslashes("--exclude 'js' --exclude 'css' --exclude 'ctools' --exclude 'imagecache' --exclude 'xmlsitemap' --exclude 'backup_migrate' --exclude 'styles' --exclude 'less'", "'"),
          'remote-files' => $aliases[$key]['remote-user'] . "@" . $aliases[$key]['remote-host'] . ":files",
        );
        $aliases[$key]['terminatur']['files']['rsync'] = $rsync;
        break;
      case 'download':
        $bucket = terminus_latest_bucket($aliases[$key]['uuid'], $aliases[$key]['env'], 'files');
        $aliases[$key]['terminatur']['files']['download-bucket'] = $bucket ? $bucket : 'NONE';
        break;
    }
  }
}

/**
 * Get a download link for code, database or files
 */
function _terminatur_get_download_link($site, $element = 'code') {
  // Refresh download link
  // @todo: Don't use import buckets for now until fixed on pantheon end
  if ($site['terminatur'][$element]['download-bucket'] == 'NONE' || strpos($site['terminatur'][$element]['download-bucket'], 'import') !== FALSE) {
    if ($element !== 'database') {
      return drush_set_error('TERMINATUR_ERROR', 'You need to make a backup on Pantheon first before you can get stuff this way.');
    }
  }
  // Make sure we are online with pantheon
  $session_data = _terminatur_get_session();
  return terminus_api_backup_download_url($site['uuid'], $site['env'], $site['terminatur'][$element]['download-bucket'], $element);
}

/**
 * Write parsed Pantheon aliases
 */
function _terminatur_write_aliases(&$aliases) {
  // Create output string
  $indent = "  ";
  $output = "<?php \n";
  $output .= "// TERMINATUR PARSED PANTHEON ALIASES \n\n";
  foreach ($aliases as $site) {
    $output .= "\$aliases['" . $site['machine-name'] . "." . $site['env'] . "'] = array(\n";
    foreach ($site as $key => $value) {
      // This needs to be TRUE, not 1
      if ($key == 'db-allows-remote') {
        $output .= $indent . "'" . $key . "' => TRUE,\n";
      }
      else {
        $output .= is_array($value) ? _terminatur_write_next_level($key, $value, $indent) : $indent . "'" . $key . "' => '" . $value . "',\n";
      }
    }
    $output .= ");\n";
  }

  // Write file
  file_put_contents(TERMINATUR_TERMINATUR_ALIASES_FILE, $output);
}

/**
 * Helper function to write parsed Pantheon aliases
 */
function _terminatur_write_next_level($label, $stuff, $indent) {
  $output = $indent . "'" . $label . "' => array(\n";
  $close = $indent . "),\n";
  $indent .= $indent;
  foreach ($stuff as $key => $value) {
    $output .= is_array($value) ? _terminatur_write_next_level($key, $value, $indent) : $indent . "'" . $key . "' => '" . $value . "',\n";
  }
  $output .= $close;
  return $output;
}
