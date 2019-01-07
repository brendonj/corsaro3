Corsaro 3: the Parallel Edition
-------------------------------

Introduction
============

Corsaro 3 is a re-implementation of a decent chunk of the original Corsaro
which aims to be better suited to processing parallel traffic sources, such
as nDAG streams or DPDK pipelines.

At present, Corsaro 3 exists (and is built) alongside the previous version
of Corsaro and any results / output produced by Corsaro 3 should be
compatible with tools or analysis programs written using the old Corsaro.


Requirements
============

 * libtrace >= 4.0.3 -- download from https://github.com/LibtraceTeam/libtrace
 * libwandio >= 1.0.4 -- download from https://github.com/wanduow/wandio
 * libyaml -- https://github.com/yaml/libyaml

Debian / Ubuntu users can also find packages for libtrace and libwandio at
http://packages.wand.net.nz/


Installing
==========

Standard installation instructions apply.


Configuration
=============

Old Corsaro used a pile of CLI arguments for configuring each run. This has
now been replaced with a YAML configuration file.

The YAML required to use Corsaro 3 is pretty simple to write. There are a
large number of top-level global config options which are simply set by
specifying a key value pair using the following format:

key: value

The plugins to apply are expressed as a YAML sequence, with the
plugin-specific options appearing as key value pairs following the
plugin name itself. For instance, the following config segment will
run a Corsaro 3 tool using the flowtuple and dos plugins. The flowtuple
plugin will be configured with the 'sorttuples' option set to 'yes'.
The dos plugin will be configured with the 'minattackduration' option set
to 60.

plugins:
 - flowtuple:
     sorttuples: yes
 - dos:
     minattackduration: 60

Note that the indentation and colon placement is important.

An example configuration file for the corsarotrace tool is included with the
Corsaro 3 source code.

Running corsarotrace
====================

Right now, the only tool implemented for Corsaro 3 is corsarotrace, which
reads packets from a libtrace input (which can be a packet trace or a live
capture interface), applies the packet processing functions of one or more
corsaro plugins to those packets, and then writes the resulting output to a
Corsaro result file.

To use corsarotrace, write a suitable config file (see below for more
details) and run the following command:

  ./corsarotrace -c <config filename> -l <logmode>

The logmode option is used to determine where any log messages that are
produced by corsarotrace end up. There are four options available:

  terminal              write log messages to stderr
  syslog                write log messages to syslog (daemon.log)
  file                  write log messages to a file
  disabled              do not write log messages


corsarotrace Config Options
===========================

The full set of supported global config options is:

  outtemplate           The template to use for output file names. The format
                        specification semantics are the same as they were in
                        old Corsaro (i.e. %P for plugin name, %N for monitor
                        name, as well as all strftime(3) modifiers).

  logfilename           If the log mode is set to 'file', the log messages
                        will be written to the file name provided by this
                        option. This option must be set if the log mode
                        is set to 'file'.

  inputuri              A libtrace URI describing where corsarotrace should
                        read captured packets from. Specify multiple 'inputuri'
                        options to have corsarotrace process multiple trace
                        files in sequence. Multiple URIs are processed in the
                        order that they appear.

  threads               Set the number of threads to be created for packet
                        processing. Defaults to 2.

  outputmode            Set to 'ascii' to have the result file written in
                        ASCII format. Set to 'binary' to have output written
                        using a custom binary format. Defaults to 'binary'.

  compressmethod        Set to 'gzip' to have the output file compressed using
                        gzip. Set to 'bz2' to have the output file compressed
                        using bzip2. Set to 'none' to not compress the output
                        file at all. Defaults to 'none'.

  compresslevel         Sets the compression level to be applied when writing
                        compressed output files. Must be a number between 1-9
                        inclusive. Defaults to 6.

  monitorid             Set the monitor name that will appear in output file
                        names if the %N modifier is present in the template.

  interval              Specifies the distribution interval length in seconds.
                        Defaults to 60.

  rotatefreq            Specifies the number of intervals that must complete
                        before the output file is rotated. Defaults to 4. If
                        set to 0, no file rotation is performed (all output
                        ends up in a single file).

  promisc               If set to 'yes', the input source will be configured
                        to use promiscuous mode (if possible). Defaults to
                        'no'.

  mergeoutput           If set to 'no', the multiple output files produced by
                        each processing thread for a rotation interval will NOT
                        be merged into a one output file per rotation interval.
                        Defaults to 'yes', which is usually the most sensible
                        option.

  startboundaryts       Ignore all packets that have a timestamp earlier than
                        the Unix timestamp given by this option.

  endboundaryts         Ignore all packets that have a timestamp after the Unix
                        timestamp given by this option.

  filter                Ignore all packets that do not match this BPF filter.

  plugins               A sequence that specifies which plugins to use for
                        packet processing, as well as any plugin-specific
                        configuration options (see below for more details).


Supported Plugins and their Configuration Options
=================================================

So far, just the flowtuple plugin has been ported to Corsaro 3, but more will
follow shortly.

Remember that plugin-specific configuration must be appear as a map within
the appearance of the plugin name in the config file, not alongside the global
options.

The flowtuple plugin only has the one option:

  sorttuples            If 'yes', the flowtuples are output in sorted order.
                        The sorting is based on the same sorting method as in
                        previous Corsaro versions. Defaults to 'yes'.



