#!/usr/bin/perl -w

BEGIN {
    if( $ENV{PERL_CORE} ) {
        chdir 't' if -d 't';
        unshift @INC, '../lib';
    }
    else {
        unshift @INC, 't/lib';
    }
}
chdir 't';

use strict;
use Test::More tests => 6;
use Data::Dumper;

BEGIN {
    use_ok( 'ExtUtils::Liblist' );
}

ok( defined &ExtUtils::Liblist::ext, 
    'ExtUtils::Liblist::ext() defined for backwards compat' );

{
    my @warn;
    local $SIG{__WARN__} = sub {push @warn, [@_]};

    my $ll = bless {}, 'ExtUtils::Liblist';
    my @out = $ll->ext('-ln0tt43r3_perl');
    is( @out, 4, 'enough output' );
    unlike( $out[2], qr/-ln0tt43r3_perl/, 'bogus library not added' );
    ok( @warn, 'had warning');

    is( grep(/\QNote (probably harmless): No library found for \E(-l)?n0tt43r3_perl/, map { @$_ } @warn), 1 ) || diag Dumper @warn;
}
