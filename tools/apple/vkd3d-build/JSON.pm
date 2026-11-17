package JSON;
# minimal stand-in for the JSON module on a host that has only core Perl: delegates to JSON::PP (core)
use strict; use warnings; use JSON::PP ();
our @ISA = ('JSON::PP');
sub import {
    my $caller = caller;
    no strict 'refs';
    *{$caller . '::decode_json'} = \&JSON::PP::decode_json;
    *{$caller . '::encode_json'} = \&JSON::PP::encode_json;
    *{$caller . '::from_json'} = sub { JSON::PP->new->decode($_[0]) };
    *{$caller . '::to_json'} = sub { JSON::PP->new->encode($_[0]) };
}
1;
