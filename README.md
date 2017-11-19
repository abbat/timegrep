# timegrep

Utility to grep log between two dates or tail last lines to time ago similar [dategrep](https://github.com/mdom/dategrep) or [dateutils](https://github.com/hroptatyr/dateutils).

## Download / Install

* [Debian, Ubuntu](http://software.opensuse.org/download.html?project=home:antonbatenev:timegrep&package=timegrep)
* [Fedora, openSUSE, CentOS](http://software.opensuse.org/download.html?project=home:antonbatenev:timegrep&package=timegrep)
* [Ubuntu PPA](https://launchpad.net/~abbat/+archive/ubuntu/timegrep) - `ppa:abbat/timegrep`
* [Arch](http://software.opensuse.org/download.html?project=home:antonbatenev:timegrep&package=timegrep), [Arch AUR](https://aur.archlinux.org/packages/timegrep/) (see also [AUR Helpers](https://wiki.archlinux.org/index.php/AUR_Helpers))
* From source code:

```
$ git clone https://github.com/abbat/timegrep.git
$ make && sudo make install
```

## How to help

* Translate this document or [man page](https://github.com/abbat/timegrep/blob/master/timegrep.1) to your native language;
* Proofreading README.md or man page with your native language;
* Share, Like, RT to your friends;
* Send PRs if you are developer.

## Usage

```
timegrep [options] [files]
```

**Options**

* `--help`, `-?` - print help message and named datetime formats;
* `--version`, `-v` - print program version and exit;
* `--format`, `-e` - datetime format (default: 'default');
* `--start`, `-f` - datetime to start search (default: now);
* `--stop`, `-t` - datetime to stop search (default: now);
* `--seconds`, `-s` - seconds to substract from `--start` (default: 0);
* `--minutes`, `-m` - minutes to substract from `--start` (default: 0);
* `--hours`, `-h` - hours to substract from `--start` (default: 0).

See [strptime(3)](https://linux.die.net/man/3/strptime) for format details. See `--help` for list of format aliases.

## Exit code

* `0` - successful completion;
* `1` - Nothing found;
* `2` - general application error.

## Examples

Grep last minute from current nginx access log (binary search):

```
$ timegrep --format=nginx --minutes=1 /var/log/nginx/access.log
```

Grep datetime interval from archive log (sequential read data from `stdin`):

```
$ zcat archive.log.gz | timegrep --start='2017:09:01 15:23:00' --stop='2017:09:01 16:32:00'
```
