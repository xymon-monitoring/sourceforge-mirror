#!/usr/bin/perl -w

#*----------------------------------------------------------------------------*/
#* Hobbit client message processor.                                           */
#*                                                                            */
#* This perl program shows how to create a server-side module using the       */
#* data sent by the Hobbit clients. This program is fed data from the         */
#* Hobbit "client" channel via the hobbitd_channel program; each client       */
#* message is processed by looking at the [who] section and generates         */
#* a "login" status that goes red when an active "root" login is found.       */
#*                                                                            */
#* Written 2007-Jan-28 by Henrik Storner <henrik@hswn.dk>                     */
#*                                                                            */
#* This program is in the public domain, and may be used freely for           */
#* creating your own Hobbit server-side modules.                              */
#*                                                                            */
#*----------------------------------------------------------------------------*/

# $Id: hobbitd_rootlogin.pl,v 1.2 2007-01-30 16:43:44 henrik Exp $


my $bb;
my $bbdisp;
my $hobbitcolumn = "login";

my $hostname = "";
my $msgtxt = "";
my %sections = ();
my $cursection = "";

sub processmessage;


# Get the BB and BBDISP environment settings.
$bb = $ENV{"BB"} || die "BB not defined";
$bbdisp = $ENV{"BBDISP"} || die "BBDISP not defined";


# Main routine. 
#
# This reads client messages from <STDIN>, looking for the
# delimiters that separate each message, and also looking for the
# section markers that delimit each part of the client message.
# When a message is complete, the processmessage() subroutine
# is invoked. $msgtxt contains the complete message, and the
# %sections hash contains the individual sections of the client 
# message.

while ($line = <STDIN>) {
	if ($line =~ /^\@\@client\#/) {
		# It's the start of a new client message - the header looks like this:
		# @@client#830759/HOSTNAME|1169985951.340108|10.60.65.152|HOSTNAME|sunos|sunos

		# Grab the hostname field from the header
		@hdrfields = split(/\|/, $line);
		$hostname = $hdrfields[3];

		# Clear the variables we use to store the message in
		$msgtxt = "";
		%sections = ();
	}
	elsif ($line =~ /^\@\@/) {
		# End of a message. Do something with it.
		processmessage();
	}
	elsif ($line =~ /^\[(.+)\]/) {
		# Start of new message section.

		$cursection = $1;
		$sections{ $cursection } = "\n";
	}
	else {
		# Add another line to the entire message text variable,
		# and the the current section.
		$msgtxt = $msgtxt . $line;
		if ($cursection) { $sections{ $cursection } = $sections{ $cursection } . $line; }
	}
}


# This subroutine processes the client message. In this case,
# we watch the [who] section of the client message and alert
# if there is a root login active.

sub processmessage {
	my $color;
	my $summary;
	my $statusmsg;
	my $cmd;

	# Dont do anything unless we have the "who" section
	return unless ( $sections{"who"} );

	# Is there a "root" login somewhere in the "who" section?
	# Note that we must match with /m because there are multiple
	# lines in the [who] section.
	if ( $sections{"who"} =~ /^root /m ) {
		$color = "red";
		$summary = "ROOT login active";
		$statusmsg = "&red ROOT login detected!\n\n" . $sections{"who"};
	}
	else {
		$color = "green";
		$summary = "OK";
		$statusmsg = "&green No root login active\n\n" . $sections{"who"};
	}

	# Build the command we use to send a status to the Hobbit daemon
	$cmd = $bb . " " . $bbdisp . " \"status " . $hostname . "." . $hobbitcolumn . " " . $color . " " . $summary . "\n\n" . $statusmsg . "\"";

	# And send the message
	system $cmd;
}

