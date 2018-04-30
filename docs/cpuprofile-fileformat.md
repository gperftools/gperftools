# Google CPU Profiler Binary Data File Format

This file documents the binary data file format produced by the Google CPU Profiler. For information about using the CPU Profiler, see [its user guide](cpuprofile.md).

The profiler source code, which generates files using this format, is at `src/profiler.cc`.

## Summary 

- [CPU Profile Data File Structure](#section-id-7)
- [Binary Header](#section-id-18)
- [Binary Profile Records](#section-id-94)
  - [Example](#section-id-125)
- [Binary Trailer](#section-id-143)
- [Text List of Mapped Objects](#section-id-170)



<div id='section-id-7'/>

## CPU Profile Data File Structure

CPU profile data files each consist of four parts, in order:

*   Binary header
*   Binary profile records
*   Binary trailer
*   Text list of mapped objects

The binary data is expressed in terms of "slots." These are words large enough to hold the program's pointer type, i.e., for 32-bit programs they are 4 bytes in size, and for 64-bit programs they are 8 bytes. They are stored in the profile data file in the native byte order (i.e., little-endian for x86 and x86_64).

<div id='section-id-18'/>

## Binary Header

The binary header format is show below. Values written by the profiler, along with requirements currently enforced by the analysis tools, are shown in parentheses.

<table summary="Header Format" frame="box" rules="sides" cellpadding="5">
<tbody>
<tr>
<th width="30%">slot</th>
<th width="70%">data</th>
</tr>
<tr>
<td>0</td>
<td>header count (0; must be 0)</td>
</tr>
<tr>
<td>1</td>
<td>header slots after this one (3; must be >= 3)</td>
</tr>
<tr>
<td>2</td>
<td>format version (0; must be 0)</td>
</tr>
<tr>
<td>3</td>
<td>sampling period, in microseconds</td>
</tr>
<tr>
<td>4</td>
<td>padding (0)</td>
</tr>
</tbody>
</table>

The headers currently generated for 32-bit and 64-bit little-endian (x86 and x86_64) profiles are shown below, for comparison.

<table summary="Header Example" frame="box" rules="sides" cellpadding="5">
<tbody>
<tr>
<th></th>
<th>hdr count</th>
<th>hdr words</th>
<th>version</th>
<th>sampling period</th>
<th>pad</th>
</tr>
<tr>
<td>32-bit or 64-bit (slots)</td>
<td>0</td>
<td>3</td>
<td>0</td>
<td>10000</td>
<td>0</td>
</tr>
<tr>
<td>32-bit (4-byte words in file)</td>
<td><tt>0x00000</tt></td>
<td><tt>0x00003</tt></td>
<td><tt>0x00000</tt></td>
<td><tt>0x02710</tt></td>
<td><tt>0x00000</tt></td>
</tr>
<tr>
<td>64-bit LE (4-byte words in file)</td>
<td><tt>0x00000 0x00000</tt></td>
<td><tt>0x00003 0x00000</tt></td>
<td><tt>0x00000 0x00000</tt></td>
<td><tt>0x02710 0x00000</tt></td>
<td><tt>0x00000 0x00000</tt></td>
</tr>
</tbody>
</table>

The contents are shown in terms of slots, and in terms of 4-byte words in the profile data file. The slot contents for 32-bit and 64-bit headers are identical. For 32-bit profiles, the 4-byte word view matches the slot view. For 64-bit profiles, each (8-byte) slot is shown as two 4-byte words, ordered as they would appear in the file.

The profiling tools examine the contents of the file and use the expected locations and values of the header words field to detect whether the file is 32-bit or 64-bit.

<div id='section-id-94'/>

## Binary Profile Records

The binary profile record format is shown below.

<table summary="Profile Record Format" frame="box" rules="sides" cellpadding="5">
<tbody>
<tr>
<th width="30%">slot</th>
<th width="70%">data</th>
</tr>
<tr>
<td>0</td>
<td>sample count, must be >= 1</td>
</tr>
<tr>
<td>1</td>
<td>number of call chain PCs (num_pcs), must be >= 1</td>
</tr>
<tr>
<td>2 .. (num_pcs + 1)</td>
<td>call chain PCs, most-recently-called function first.</td>
</tr>
</tbody>
</table>

The total length of a given record is 2 + num_pcs.

Note that multiple profile records can be emitted by the profiler having an identical call chain. In that case, analysis tools should sum the counts of all records having identical call chains.

**Note:** Some profile analysis tools terminate if they see _any_ profile record with a call chain with its first entry having the address 0\. (This is similar to the binary trailer.)

<div id='section-id-125'/>

### Example

This example shows the slots contained in a sample profile record.

<table summary="Profile Record Example" frame="box" rules="sides" cellpadding="5">
<tbody>
<tr>
<td>5</td>
<td>3</td>
<td>0xa0000</td>
<td>0xc0000</td>
<td>0xe0000</td>
</tr>
</tbody>
</table>

In this example, 5 ticks were received at PC 0xa0000, whose function had been called by the function containing 0xc0000, which had been called from the function containing 0xe0000.

<div id='section-id-143'/>

## Binary Trailer

The binary trailer consists of three slots of data with fixed values, shown below.

<table summary="Trailer Format" frame="box" rules="sides" cellpadding="5">
<tbody>
<tr>
<th width="30%">slot</th>
<th width="70%">value</th>
</tr>
<tr>
<td>0</td>
<td>0</td>
</tr>
<tr>
<td>1</td>
<td>1</td>
</tr>
<tr>
<td>2</td>
<td>0</td>
</tr>
</tbody>
</table>

Note that this is the same data that would contained in a profile record with sample count = 0, num_pcs = 1, and a one-element call chain containing the address 0.

<div id='section-id-170'/>

## Text List of Mapped Objects

The binary data in the file is followed immediately by a list of mapped objects. This list consists of lines of text separated by newline characters.

Each line is one of the following types:

*   Build specifier, starting with "<tt>build=</tt>". For example:

    <pre>  build=/path/to/binary</pre>

    Leading spaces on the line are ignored.
*   Mapping line from ProcMapsIterator::FormatLine. For example:

    <pre>  40000000-40015000 r-xp 00000000 03:01 12845071   /lib/ld-2.3.2.so</pre>

    The first address must start at the beginning of the line.

Unrecognized lines should be ignored by analysis tools.

When processing the paths see in mapping lines, occurrences of <tt>$build</tt> followed by a non-word character (i.e., characters other than underscore or alphanumeric characters), should be replaced by the path given on the last build specifier line.

---

<address>
Chris Demetriou  
Last modified: Feb 2018
</address>

[Link to main documentation readme](readme.md)
