#
# A simple shell script to authenticate users against LDAP.
#
# This script should be able to run on any POSIX-compliant shell with
# cat, grep and sed available (even works on BusyBox).
# It expects to get two environment variables: username and password of
# the user to be authenticated.
#
# It then rewrites the username into a DN and tries to bind with it,
# using the supplied password.
#
# When binding succeded, it can optionally execute a search to e.g. check
# for group memberships or, alternatively, grant access straight away.
#
# When access is granted, it exits with an exit code of 0, non-zero
# otherwise. Status messages are only written to stderr, not stdout,
# so you can use stdout to pass whatever you want back to the caller.
#
# NOTE: Don't call this script directly!
# Source it instead, set configuration parameters as desired and call the
# ldap_auth_run function, like so:
#
#     #!/bin/sh
#     . "$(dirname "$0")/ldap-auth.inc"
#     # Configuration...
#     ldap_auth_run
#
# It'll then go, do the authentication with your custom settings and exit.
#
# Additionally, you may define the following functions in your own script,
# which get then called at the corresponding events, before the program
# exits. You could print some additional info from these functions,
# for instance.
#
#     on_auth_success() {
#         ...
#     }
#
#     on_auth_failure() {
#         ...
#     }
#
# In these functions, you have access to the $output variable,
# which will hold the raw LDIF output produced by the LDAP client.
# It won't include comments, but curl produces a somewhat invalid LDIF
# with blank lines between different attributes and deep indentation.
#

########## CONFIGURATION ##########

# Some settings (those not commented out) have default values set. You
# don't need to specify them yourself unless you want to change these
# defaults.

# Uncomment to enable debugging to stderr (prints full client output and more).
#DEBUG=1

# Must be one of "curl" and "ldapsearch".
# NOTE:
# - When choosing "curl", make sure "curl --version | grep ldap" outputs
#   something. Otherwise, curl was compiled without LDAP support.
# - When choosing "ldapsearch", make sure the ldapwhoami command is
#   available as well, as that might be needed in some cases.
#CLIENT="curl"

# Usernames should be validated to be of a known format.
# Special characters will be escaped anyway, but it is generally not
# recommended to allow more than necessary.
# You could use something like this.
#USERNAME_PATTERN='^[a-z|A-Z|0-9|_|-|.]+$'

# Adapt to your needs.
#SERVER="ldap://ldap-server:389"
# Will try binding as this user.
# ldap_dn_escape escapes special characters in strings to make them
# usable within LDAP DN components.
#USERDN="uid=$(ldap_dn_escape "$username"),ou=people,dc=example,dc=com"

# If you want to take additional checks like requiring group memberships,
# you can execute a custom search, which has to return exactly one result
# in order for authentication to succeed.
# Uncomment the following lines to enable search query execution.
#BASEDN="$USERDN"
#SCOPE="base"
#FILTER="(&(objectClass=person)(memberOf=cn=some-group,ou=groups,dc=example,dc=com))"
# Space-separated list of additional LDAP attributes to query.
# You could process them in your own on_auth_success hook.
#ATTRS="cn"

# When the timeout (in seconds) is exceeded (e.g. due to slow networking),
# authentication fails.
TIMEOUT=3

########## END OF CONFIGURATION ##########


# Log messages to stderr.
log() {
	echo "$1" >&2
}


# Escape string to be safely usable in LDAP DN components and URIs.
# https://ldapwiki.com/wiki/DN%20Escape%20Values
ldap_dn_escape() {
	escaped=$(echo "$1" | sed -r \
		-e 's/[,\\#+<>;"=/?]/\\\0/g' \
		-e 's/^ (.*)$/\\ \1/' \
		-e 's/^(.*) $/\1\\ /' \
	)
	[ -z "$DEBUG" ] || log "Escaped '$1' to '$escaped'."
	echo "$escaped"
}


# The different client implementations.
ldap_auth_curl() {
	[ -z "$DEBUG" ] || verbose="-v"
	attrs=$(echo "$ATTRS" | sed "s/ /,/g")
	output=$(curl $verbose -s -m "$TIMEOUT" -u "$USERDN:$password" \
		"$SERVER/$BASEDN?dn,$attrs?$SCOPE?$FILTER")
	[ $? -ne 0 ] && return 1
	return 0
}

ldap_auth_ldapsearch() {
	common_opts="-o nettimeout=$TIMEOUT -H $SERVER -x"
	[ -z "$DEBUG" ] || common_opts="-v $common_opts"
	if [ -z "$BASEDN" ]; then
		output=$(ldapwhoami $common_opts -D "$USERDN" -w "$password")
	else
		output=$(ldapsearch $common_opts -LLL \
			-D "$USERDN" -w "$password" \
			-s "$SCOPE" -b "$BASEDN" "$FILTER" dn $ATTRS)
	fi
	[ $? -ne 0 ] && return 1
	return 0
}


# Entrypoint
ldap_auth_run() {
	# Check username and password are present and not malformed.
	if [ -z "$username" ] || [ -z "$password" ]; then
		log "Need username and password environment variables."
		exit 2
	fi
	if [ ! -z "$USERNAME_PATTERN" ]; then
		username_match=$(echo "$username" | sed -r "s/$USERNAME_PATTERN/x/")
		if [ "$username_match" != "x" ]; then
			log "Username '$username' has an invalid format."
			exit 2
		fi
	fi

	if [ -z "$SERVER" ] || [ -z "$USERDN" ]; then
		log "SERVER and USERDN need to be configured."
		exit 2
	fi
	if [ -z "$TIMEOUT" ]; then
		log "TIMEOUT needs to be configured."
		exit 2
	fi
	if [ ! -z "$BASEDN" ]; then
		if [ -z "$SCOPE" ] || [ -z "$FILTER" ]; then
			log "BASEDN, SCOPE and FILTER may only be configured together."
			exit 2
		fi
	elif [ ! -z "$ATTRS" ]; then
		log "Configuring ATTRS only makes sense when enabling searching."
		exit 2
	fi

	case "$CLIENT" in
		"curl")
			ldap_auth_curl
			;;
		"ldapsearch")
			ldap_auth_ldapsearch
			;;
		*)
			log "Unsupported client '$CLIENT', revise the configuration."
			exit 2
			;;
	esac

	result=$?

	entries=0
	if [ $result -eq 0 ] && [ ! -z "$BASEDN" ]; then
		entries=$(echo "$output" | grep -cie '^dn\s*:')
		[ "$entries" != "1" ] && result=1
	fi

	if [ ! -z "$DEBUG" ]; then
		cat >&2 <<-EOF
			Result: $result
			Number of entries: $entries
			Client output:
			$output
			EOF
	fi

	if [ $result -ne 0 ]; then
		log "User '$username' failed to authenticate."
		type on_auth_failure > /dev/null && on_auth_failure
		exit 1
	fi

	log "User '$username' authenticated successfully."
	type on_auth_success > /dev/null && on_auth_success
	exit 0
}