Name:          timegrep
Version:       0.5
Release:       1
Summary:       Utility to grep log between two dates or tail last lines to time ago
Group:         Productivity/Text/Utilities
License:       BSD-2-Clause
URL:           https://github.com/abbat/timegrep
BuildRequires: pcre-devel
Source0:       https://build.opensuse.org/source/home:antonbatenev:timegrep/timegrep/timegrep_%{version}.tar.bz2
BuildRoot:     %{_tmppath}/%{name}-%{version}-build


%description
Utility to grep log between two dates or tail last
lines to time ago similar dategrep.


%prep
%setup -q -n timegrep


%build
make USER_CFLAGS="${RPM_OPT_FLAGS}" USER_CPPFLAGS="${RPM_OPT_FLAGS}" USER_LDFLAGS="${RPM_LD_FLAGS}" %{?_smp_mflags}


%install

install -d %{buildroot}%{_bindir}

install -m755 timegrep %{buildroot}%{_bindir}/timegrep

install -d %{buildroot}%{_mandir}/man1

install -m644 timegrep.1 %{buildroot}%{_mandir}/man1/timegrep.1


%clean
rm -rf %{buildroot}


%files
%defattr(-,root,root,-)
%doc README.md
%doc %{_mandir}/man1/timegrep.1*

%{_bindir}/timegrep


%changelog
* Tue Jul 24 2018 Anton Batenev <antonbatenev@yandex.ru> 0.5-1
- Initial RPM release
