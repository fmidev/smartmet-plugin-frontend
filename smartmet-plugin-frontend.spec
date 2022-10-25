%define DIRNAME frontend
%define SPECNAME smartmet-plugin-%{DIRNAME}
Summary: SmartMet frontend plugin
Name: %{SPECNAME}
Version: 22.10.25
Release: 1%{?dist}.fmi
License: MIT
Group: SmartMet/Plugins
URL: https://github.com/fmidev/smartmet-plugin-frontend
Source0: %{name}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
%if 0%{?rhel} && 0%{rhel} < 9
%define smartmet_boost boost169
%else
%define smartmet_boost boost
%endif

%define smartmet_fmt_min 8.1.1
%define smartmet_fmt_max 8.2.0

BuildRequires: rpm-build
BuildRequires: gcc-c++
BuildRequires: make
BuildRequires: %{smartmet_boost}-devel
BuildRequires: smartmet-library-timeseries-devel >= 22.10.4
BuildRequires: smartmet-library-spine-devel >= 22.9.5
BuildRequires: smartmet-library-grid-files-devel >= 22.9.29
BuildRequires: smartmet-engine-sputnik-devel >= 22.10.5
BuildRequires: gdal34-devel
BuildRequires: jsoncpp-devel
BuildRequires: protobuf-devel
BuildRequires: smartmet-library-macgyver-devel >= 22.8.23
Requires: protobuf
Requires: smartmet-library-macgyver >= 22.8.23
Requires: smartmet-server >= 22.8.19
Requires: smartmet-engine-sputnik >= 22.10.5
Requires: smartmet-library-spine >= 22.9.5
Requires: smartmet-library-timeseries >= 22.10.4
Requires: smartmet-library-grid-files >= 22.9.29
Requires: jsoncpp
%if 0%{rhel} >= 7
Requires: %{smartmet_boost}-date-time
Requires: %{smartmet_boost}-thread
%endif
Provides: %{SPECNAME}
Obsoletes: smartmet-brainstorm-frontend < 16.11.1
Obsoletes: smartmet-brainstorm-frontend-debuginfo < 16.11.1

%description
SmartMet frontend plugin

%prep
rm -rf $RPM_BUILD_ROOT

%setup -q -n %{SPECNAME}
 
%build -q -n %{SPECNAME}
make %{_smp_mflags}

%install
%makeinstall

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(0775,root,root,0775)
%{_datadir}/smartmet/plugins/%{DIRNAME}.so

%changelog
* Tue Oct 25 2022 Andris Pavenis <andris.pavenis@fmi.fi> 22.10.25-1.fmi
- [BRAINSTORM-2445] Hot-fix to support URLs in format /backend_name/...

* Wed Oct  5 2022 Mika Heiskanen <mika.heiskanen@fmi.fi> - 22.10.5-1.fmi
- Use modified Sputnik API to get client IP to error logs

* Thu Sep 15 2022 Mika Heiskanen <mika.heiskanen@fmi.fi> - 22.9.15-2.fmi
- Add ETag to cached responses

* Wed Sep  7 2022 Andris Pavenis <andris.pavenis@fmi.fi> 22.9.7-1.fmi
- Support using URI prefixes (like /edr/...)

* Thu Aug 25 2022 Mika Heiskanen <mika.heiskanen@fmi.fi> - 22.8.25-1.fmi
- Use a generic exception handler for configuration file errors

* Wed Jul 27 2022 Mika Heiskanen <mika.heiskanen@fmi.fi> - 22.7.27-1.fmi
- Repackaged since macgyver CacheStats ABI changed

* Tue Jun 21 2022 Andris Pavēnis <andris.pavenis@fmi.fi> 22.6.21-1.fmi
- Add support for RHEL9, upgrade libpqxx to 7.7.0 (rhel8+) and fmt to 8.1.1

* Tue May 24 2022 Mika Heiskanen <mika.heiskanen@fmi.fi> - 22.5.24-1.fmi
- Repackaged due to NFmiArea ABI changes

* Mon May 23 2022 Mika Heiskanen <mika.heiskanen@fmi.fi> - 22.5.23-1.fmi
- Removed unnecessary linkage to newbase

* Mon Mar 21 2022 Andris Pavēnis <andris.pavenis@fmi.fi> 22.3.21-1.fmi
- Update due to changes in smartmet-library-spine and smartnet-library-timeseries

* Thu Mar 10 2022 Mika Heiskanen <mika.heiskanen@fmi.fi> - 22.3.10-1.fmi
- Repackaged due to refactored librariy dependencies

* Thu Jan 27 2022 Mika Heiskanen <mika.heiskanen@fmi.fi> - 22.1.27-1.fmi
- Added modification time to status requests

* Mon Jan 24 2022 Andris Pavēnis <andris.pavenis@fmi.fi> 22.1.24-1.fmi
- Repackage due to upgrade from PGDG (gdal 4.3 etc)

* Thu Jan 20 2022 Mika Heiskanen <mika.heiskanen@fmi.fi> - 22.1.20-1.fmi
- Added gridgenerations and gridgenerationsqd admin queries

* Mon Sep 13 2021 Mika Heiskanen <mika.heiskanen@fmi.fi> - 21.9.13-1.fmi
- Repackaged due to Fmi::Cache statistics fixes

* Tue Aug 31 2021 Mika Heiskanen <mika.heiskanen@fmi.fi> - 21.8.31-1.fmi
- Improved cache statistics output

* Mon Aug 30 2021 Anssi Reponen <anssi.reponen@fmi.fi> - 21.8.30-1.fmi
- Cache counters added (BRAINSTORM-1005)

* Tue Aug 17 2021 Mika Heiskanen <mika.heiskanen@fmi.fi> - 21.8.17-1.fmi
- Use the new shutdown API

* Thu Apr 22 2021 Anssi Reponen <anssi.reponen@fmi.fi> - 21.4.22-4.fmi
- Added what=list query (BRAINSTORM-2030)

* Tue Apr 20 2021 Mika Heiskanen <mika.heiskanen@fmi.fi> - 21.4.20-4.fmi
- Improved backend request counting

* Tue Apr 20 2021 Mika Heiskanen <mika.heiskanen@fmi.fi> - 21.4.20-1.fmi
- Fixed backend request counters not to leak on errors

* Thu Feb 25 2021 Mika Heiskanen <mika.heiskanen@fmi.fi> - 21.2.25-1.fmi
- Use a much smaller response cache, it was severely oversized

* Thu Jan 14 2021 Mika Heiskanen <mika.heiskanen@fmi.fi> - 21.1.14-1.fmi
- Repackaged smartmet to resolve debuginfo issues

* Mon Dec  7 2020 Mika Heiskanen <mika.heiskanen@fmi.fi> - 20.12.7-1.fmi
- Minor fixes to silence CodeChecker warnings

* Wed Oct 14 2020 Mika Heiskanen <mika.heiskanen@fmi.fi> - 20.10.14-1.fmi
- Use new TableFormatter API

* Mon Oct 12 2020 Mika Heiskanen <mika.heiskanen@fmi.fi> - 20.10.12-1.fmi
- Prefer lambdas over boost::bind

* Tue Oct  6 2020 Mika Heiskanen <mika.heiskanen@fmi.fi> - 20.10.6-1.fmi
- Enable sensible relative libconfig include paths

* Wed Sep 23 2020 Mika Heiskanen <mika.heiskanen@fmi.fi> - 20.9.23k8.21-1.fmi
- Use Fmi::Exception instead of Spine::Exception

* Fri Aug 21 2020 Mika Heiskanen <mika.heiskanen@fmi.fi> - 20.8.21-1.fmi
- Upgrade to fmt 6.2

* Sat Apr 18 2020 Mika Heiskanen <mika.heiskanen@fmi.fi> - 20.4.18-1.fmi
- Upgraded to Boost 1.69

* Tue Mar  3 2020 Mika Heiskanen <mika.heiskanen@fmi.fi> - 20.3.3-1.fmi
- Scan all backends for querydata content without knowing if the admin plugin is installed or not

* Fri Dec 27 2019 Mika Heiskanen <mika.heiskanen@fmi.fi> - 19.12.27-2.fmi
- Added timeformat setting for qengine status queries

* Fri Dec 27 2019 Mika Heiskanen <mika.heiskanen@fmi.fi> - 19.12.27-1.fmi
- Admin qengine status request can now be limited to a particular producer

* Wed Nov 20 2019 Mika Heiskanen <mika.heiskanen@fmi.fi> - 19.11.20-1.fmi
- Repackaged since Spine::Parameter size changed

* Thu Sep 26 2019 Mika Heiskanen <mika.heiskanen@fmi.fi> - 19.9.26-1.fmi
- Repackaged due to ABI changes

* Thu Feb 28 2019 Mika Heiskanen <mika.heiskanen@fmi.fi> - 19.2.28-2.fmi
- Fixed frontend to cache Access-Control-Allow-Origin headers

* Thu Feb 28 2019 Mika Heiskanen <mika.heiskanen@fmi.fi> - 19.2.28-1.fmi
- Added client IP to active requests report

* Thu Dec 13 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.12.13-1.fmi
- Added password protection for admin requests
- Added "duration" option for pause/continue requests

* Wed Dec 12 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.12.12-2.fmi
- Implemented pause and continue requests

* Tue Dec  4 2018 Pertti Kinnia <pertti.kinnia@fmi.fi> - 18.12.4-1.fmi
- Repackaged since Spine::Table size changed

* Mon Nov  5 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.11.5-1.fmi
- Improved error handling
- Added tracking of the number of active requests to each backend

* Thu Oct  4 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.10.4-1.fmi
- Filesystem cache directory setting can now be unset if the configured size is zero

* Mon Sep 10 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.9.10-1.fmi
- Silenced last CodeChecker warnings

* Sun Aug 26 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.8.26-1.fmi
- Silenced CodeChecker warnings

* Thu Aug 16 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.8.16-1.fmi
- Code cleanup

* Mon Aug 13 2018 Pertti Kinnia <pertti.kinnia@fmi.fi> - 18.8.13-1.fmi
- Use 'admin' as service name for qengine backend detection; pointforecast -plugin is not necessarily enabled (BS-1268)

* Wed Aug  8 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.8.8-1.fmi
- Silenced more CodeChecker warnings

* Fri Aug  3 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.8.3-1.fmi
- Silenced CodeChecker warnings

* Fri Jul 27 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.7.27-1.fmi
- Faster formatting of response header dates

* Tue Jun 19 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.6.19-1.fmi
- Prevent crashes if common content for some producer is empty

* Mon Jun 11 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.6.11-1.fmi
- Made backend timeout configurable using backend.timeout. Default is 600 seconds
- Made backend thread count configurable using backend.threads. Default is 20.

* Mon May 14 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.5.14-1.fmi
- Added handling of high_load response code 1234

* Wed May  9 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.4.9-1.fmi
- Added an admin query listing active requests

* Sat Apr  7 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.4.7-1.fmi
- Upgrade to boost 1.66

* Wed Mar 21 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.3.21-1.fmi
- SmartMetCache ABI changed

* Tue Mar 20 2018 Mika Heiskanen <mika.heiskanen@fmi.fi> - 18.3.20-1.fmi
- Full recompile of all server plugins

* Fri Dec  1 2017 Mika Heiskanen <mika.heiskanen@fmi.fi> - 17.12.1-1.fmi
- Send cache headers also for cached responses as requested by RFC7232

* Fri Nov 24 2017 Mika Heiskanen <mika.heiskanen@fmi.fi> - 17.11.24-1.fmi
- Improved cache header handling

* Thu Sep  7 2017 Mika Heiskanen <mika.heiskanen@fmi.fi> - 17.9.7-1.fmi
- Add server name to cached responses

* Mon Aug 28 2017 Mika Heiskanen <mika.heiskanen@fmi.fi> - 17.8.28-1.fmi
- Upgrade to boost 1.65

* Tue Apr 11 2017 Mika Heiskanen <mika.heiskanen@fmi.fi> - 17.4.11-1.fmi
- Enable CORS by allowing Access-Control-Allow-Origin for all hosts

* Sat Apr  8 2017 Mika Heiskanen <mika.heiskanen@fmi.fi> - 17.4.8-1.fmi
- Simplified error reporting

* Thu Mar 16 2017 Mika Heiskanen <mika.heiskanen@fmi.fi> - 17.3.16-1.fmi
- Switched from json_spirit to jsoncpp

* Wed Mar 15 2017 Mika Heiskanen <mika.heiskanen@fmi.fi> - 17.3.15-1.fmi
- Recompiled since Spine::Exception changed

* Wed Jan  4 2017 Mika Heiskanen <mika.heiskanen@fmi.fi> - 17.1.4-1.fmi
- Changed to use renamed SmartMet base libraries

* Wed Nov 30 2016 Mika Heiskanen <mika.heiskanen@fmi.fi> - 16.11.30-1.fmi
- No installation for configuration

* Tue Nov  1 2016 Mika Heiskanen <mika.heiskanen@fmi.fi> - 16.11.1-1.fmi
- Namespace changed

* Thu Sep 29 2016 Mika Heiskanen <mika.heiskanen@fmi.fi> - 16.9.29-1.fmi
- Bug fix: do not let async operations exit by throwing

* Tue Sep  6 2016 Mika Heiskanen <mika.heiskanen@fmi.fi> - 16.9.6-1.fmi
- New exception handler
- Fixed configuration file parser error messages to refer to Frontend, not Dali

* Tue Aug 30 2016 Mika Heiskanen <mika.heiskanen@fmi.fi> - 16.8.30-1.fmi
- Base class API change
- Use response code 400 instead of 503

* Fri Aug 26 2016 Mika Heiskanen <mika.heiskanen@fmi.fi> - 16.8.26-1.fmi
- Resend requests to another backend on shutdown only, not if the backend has crashed

* Tue Aug 23 2016 Mika Heiskanen <mika.heiskanen@fmi.fi> - 16.8.23-1.fmi
- The internal HTTP response code for shutdown is now 3210, not 503

* Mon Aug 15 2016 Markku Koskela <markku.koskela@fmi.fi> - 16.8.15-1.fmi
- The init(),shutdown() and requestHandler() methods are now protected methods
- The requestHandler() method is called from the callRequestHandler() method
- If the frontend receives an HTTP message which status is 503 from the backend
- then it tries automatically to forward the request to another backend server.
- There are some modifications related to the management of the backend server
- lists. Now the backend server identification requires also the port number, because
- serveral servers can run in the same computer.

* Tue Jun 14 2016 Mika Heiskanen <mika.heiskanen@fmi.fi> - 16.6.14-1.fmi
- Full recompile

* Thu Jun  2 2016 Mika Heiskanen <mika.heiskanen@fmi.fi> - 16.6.2-1.fmi
- Full recompile

* Wed Jun  1 2016 Mika Heiskanen <mika.heiskanen@fmi.fi> - 16.6.1-1.fmi
- Added graceful shutdown

* Tue May 31 2016 Mika Heiskanen <mika.heiskanen@fmi.fi> - 16.5.31-1.fmi
- Fixed a memory leak in generating the response mime type

* Tue May 17 2016 Tuomo Lauri <tuomo.lauri@fmi.fi> - 16.5.17-1.fmi
- Now discouraging proxy caching with appropriate headers

* Tue Jan 26 2016 Mika Heiskanen <mika.heiskanen@fmi.fi> - 16.1.26-1.fmi
- spine API changes

* Mon Jan 18 2016 Mika Heiskanen <mika.heiskanen@fmi.fi> - 16.1.18-1.fmi
- newbase API changed, full recompile

* Fri Dec 11 2015 Tuomo Lauri <tuomo.lauri@fmi.fi> - 15.12.11-1.fmi
- Fixed socket leak issue when replying with cached responses

* Wed Nov 18 2015 Tuomo Lauri <tuomo.lauri@fmi.fi> - 15.11.18-1.fmi
- SmartMetPlugin now receives a const HTTP Request

* Mon Oct 26 2015 Mika Heiskanen <mika.heiskanen@fmi.fi> - 15.10.26-1.fmi
- Added proper debuginfo packaging

* Fri Aug 28 2015 Tuomo Lauri <tuomo.lauri@fmi.fi> - 15.8.28-1.fmi
- Rebuilt against the new Spine

* Mon Aug 24 2015 Mika Heiskanen <mika.heiskanen@fmi.fi> - 15.8.24-1.fmi
- Recompiled due to Convenience.h API changes

* Tue Aug 18 2015 Mika Heiskanen <mika.heiskanen@fmi.fi> - 15.8.18-1.fmi
- Recompile forced by brainstorm API changes

* Mon Aug 17 2015 Mika Heiskanen <mika.heiskanen@fmi.fi> - 15.8.17-1.fmi
- Use -fno-omit-frame-pointer to improve perf use 

* Mon Jul 27 2015 Tuomo Lauri <tuomo.lauri@fmi.fi> - 15.7.27-1.fmi
- Now setting no_delay to backend socket

* Mon Apr 13 2015 Tuomo Lauri <tuomo.lauri@fmi.fi> - 15.4.13-1.fmi
- Fixed race condition in timeout timer

* Thu Apr  9 2015 Mika Heiskanen <mika.heiskanen@fmi.fi> - 15.4.9-1.fmi
- newbase API changed

* Wed Apr  8 2015 Mika Heiskanen <mika.heiskanen@fmi.fi> - 15.4.8-1.fmi
- Using dynamic linking for smartmet libraries

* Thu Mar 26 2015 Tuomo Lauri <tuomo.lauri@fmi.fi> - 15.3.26-1.fmi
- Improved Frontend threading model

* Tue Mar 24 2015 Tuomo Lauri <tuomo.lauri@fmi.fi> - 15.3.24-2.fmi
- Using shared_from_this to discourage segfaults

* Tue Mar 24 2015 Tuomo Lauri <tuomo.lauri@fmi.fi> - 15.3.24-1.fmi
- Added constraint response status == ok to caching

* Mon Mar 23 2015 Tuomo Lauri <tuomo.lauri@fmi.fi> - 15.3.23-1.fmi
- Implemented ETag-based backend response caching

* Wed Feb 25 2015 Tuomo Lauri <tuomo.lauri@fmi.fi> - 15.2.25-1.fmi
- Removed a lot of unncecssary cruft from HTTP forwarding
- Incoming requests are now forwarder as-is, instead of rebuilding them from scratch

* Thu Dec 18 2014 Mika Heiskanen <mika.heiskanen@fmi.fi> - 14.12.18-1.fmi
- Recompiled due to spine API changes

* Tue Jul 1 2014 Mikko Visa <mikko.visa@fmi.fi> - 14.7.1-1.fmi
- Recompile because of API changes

* Fri May 16 2014 Mika Heiskanen <mika.heiskanen@fmi.fi> - 14.5.14-1.fmi
- Fixed bug in proxy response status setting

* Wed May 14 2014 Mika Heiskanen <mika.heiskanen@fmi.fi> - 14.5.14-1.fmi
- Use shared macgyver and locus libraries

* Mon Apr 28 2014 Mika Heiskanen <mika.heiskanen@fmi.fi> - 14.4.28-1.fmi
- Full recompile due to large changes in spine etc APIs

* Tue Jan 14 2014 Tuomo Lauri <tuomo.lauri@fmi.fi> - 14.1.14-1.fmi
- Recompile due to changes in Spine

* Tue Nov  5 2013 Mika Heiskanen <mika.heiskanen@fmi.fi> - 13.11.5-1.fmi
- Updated dependency (maybe unnecessarily)
- Frontend now tries to forward requests to another backend if backend connection fails

* Wed Oct  9 2013 Tuomo Lauri <tuomo.lauri@fmi.fi> - 13.10.9-1.fmi
- Now conforming with the new Reactor initialization API
- Built against the new Spine

* Fri Sep 20 2013 Tuomo Lauri    <tuomo.lauri@fmi.fi>    - 13.9.20-1.fmi
- Slight adjustments to reduce reported errors at Frontend

* Wed Sep 18 2013 Tuomo Lauri    <tuomo.lauri@fmi.fi>    - 13.9.18-1.fmi
- Backend calls no longer block the calling threads
- Backend connection timeoutting implemented

* Fri Sep 6  2013 Tuomo Lauri    <tuomo.lauri@fmi.fi>    - 13.9.6-1.fmi
- Recompiled due Spine changes

* Fri Aug 16 2013 Tuomo Lauri    <tuomo.lauri@fmi.fi>    - 13.8.16-1.fmi
- Added explicit reporting when backend is removed due to throttling

* Thu Aug 15 2013 Tuomo Lauri    <tuomo.lauri@fmi.fi>    - 13.8.15-1.fmi
- Proxy now sets the backend to which forwarding was done to the Response object
- HTTP now informs backend sentinels when forwarding requests

* Mon Aug 12 2013 Mika Heiskanen <mika.heiskanen@fmi.fi> - 13.8.12-1.fmi
- Added connection throttling

* Tue Jul 23 2013 Mika Heiskanen <mika.heiskanen@fmi.fi> - 13.7.23-1.fmi
- Recompiled due to thread safety fixes in newbase & macgyver

* Wed Jul  3 2013 mheiskan <mika.heiskanen@fmi.fi> - 13.7.3-1.fmi
- Update to boost 1.54

* Wed May 15 2013 lauri <tuomo.lauri@fmi.fi> - 13.5.15-1.fmi
- Added apikey header forwarding 

* Tue Apr 23 2013 lauri <tuomo.lauri@fmi.fi> - 13.4.23-1.fmi
- Fixed possible race condition in gateway streamer

* Mon Apr 22 2013 mheiskan <mika.heiskanen@fi.fi> - 13.4.22-1.fmi
- Brainstorm API changed

* Fri Apr 12 2013 lauri <tuomo.lauri@fmi.fi>    - 13.4.12-1.fmi
- Rebuild due to changes in Spine

* Tue Apr  9 2013 mheiskan <mika.heiskanen@fmi.fi> - 13.4.9-1.fmi
- Print a readable error message when a system call sets errno

* Thu Apr  4 2013 lauri    <tuomo.lauri@fmi.fi>    - 13.4.4-1.fmi
- Removed gethostbyaddr from Proxy, since Sputnik already provides the required information

* Wed Apr  3 2013 lauri    <tuomo.lauri@fmi.fi>    - 13.4.3-2.fmi
- Added error reporting to Proxy class

* Wed Apr  3 2013 lauri    <tuomo.lauri@fmi.fi>    - 13.4.3-1.fmi
- Added interruption point to frontend backend socket reading function

* Tue Mar 26 2013 lauri    <tuomo.lauri@fmi.fi>    - 13.3.26-1.fmi
- Fixed a bug which caused a possible deadlock in frontend if user aborts the connection

* Fri Mar 22 2013 lauri    <tuomo.lauri@fmi.fi>    - 13.3.22-1.fmi
- Fixed bug in Frontend streaming, streaming now significantly faster
- Extended IP filtering to all registered plugins

* Mon Mar 11 2013 lauri    <tuomo.lauri@fmi.fi>    - 13.3.11-1.fmi
- Added IP filtering to restrict access to admin functionality

* Wed Feb  6 2013 lauri    <tuomo.lauri@fmi.fi>    - 13.2.6-1.fmi
- Added streaming capabilities from the new server
- Built against new Spine and Server

* Wed Nov  7 2012 mheiskan <mika.heiskanen@fmi.fi> - 12.11.7-1.fmi
- Upgrade to boost 1.52
- Upgrade to refactored spine library

* Tue Oct 16 2012 lauri    <tuomo.lauri@fmi.fi>    - 12.10.16-1.el6.fmi
- Added minimum and maximum times to content reporting

* Thu Aug 30 2012 lauri    <tuomo.lauri@fmi.fi>    - 12.8.30-1.el6.fmi
- Added backend QEngine content reporting

* Wed Aug 15 2012 mheiskan <mika.heiskanen@fmi.fi> - 12.8.15-1.el6.fmi
- Mime type fixes
- Removed HTML formatting from formats other than debug

* Thu Aug  9 2012 mheiskan <mika.heiskanen@fmi.fi> - 12.8.9-1.el6.fmi
- Added possibility to request list of backend machines

* Tue Aug  7 2012 mheiskan <mika.heiskanen@fmi.fi> - 12.8.7-2.el6.fmi
- Added wall clock times to cout messages

* Tue Aug 7  2012 lauri    <tuomo.lauri@fmi.fi>    - 12.8.7-1.el6.fmi
- Added support for WWW-authenticate

* Wed Jul 25 2012 mheiskan <mika.heiskanen@fmi.fi> - 12.7.25-1.el6.fmi
- Reimplemented clusterinfo query

* Mon Jul  9 2012 mheiskan <mika.heiskanen@fmi.fi> - 12.7.9-1.el6.fmi
- Fixed to use getSingleton for accessing engines

* Thu Jul  5 2012 mheiskan <mika.heiskanen@fmi.fi> - 12.7.5-2.el6.fmi
- Modified to use improved Sputnik API

* Thu Jul  5 2012 mheiskan <mika.heiskanen@fmi.fi> - 12.7.5-1.el6.fmi
- Upgrade to boost 1.50

* Wed Apr  4 2012 mheiskan <mika.heiskanen@fmi.fi> - 12.4.4-1.el6.fmi
- full recompile due to common lib change

* Mon Apr  2 2012 mheiskan <mika.heiskanen@fmi.fi> - 12.4.2-1.el6.fmi
- macgyver change forced recompile

* Sat Mar 31 2012 mheiskan <mika.heiskanen@fmi.fi> - 12.3.31-1.el5.fmi
- Upgrade to boost 1.49

* Thu Dec 29 2011 mheiskan <mika.heiskanen@fmi.fi> - 11.12.29-1.el5.fmi
- Forward Accept-Encoding and If-Modified-Since headers to backend

* Wed Dec 21 2011 mheiskan <mika.heiskanen@fmi.fi> - 11.12.21-1.el6.fmi
- RHEL6 release

* Tue Aug 16 2011 mheiskan <mika.heiskanen@fmi.fi> - 11.8.16-1.el5.fmi
- Upgrade to boost 1.47

* Thu Mar 24 2011 mheiskan <mika.heiskanen@fmi.fi> - 11.3.24-1.el5.fmi
- Upgrade to boost 1.46

* Thu Oct 28 2010 mheiskan <mika.heiskanen@fmi.fi> - 10.10.28-1.el5.fmi
- Recompiled due to external API changes

* Tue Sep 14 2010 mheiskan <mika.heiskanen@fmi.fi> - 10.9.14-1.el5.fmi
- Upgrade to boost 1.44

* Mon Aug 16 2010 westerba <antti.westerberg@fmi.fi> - 10.8.16-1.el5.fmi
- Changed the plugin name to frontend (this is the plugins real functionality)

* Fri Jan 15 2010 mheiskan <mika.heiskanen@fmi.fi> - 10.1.15-1.el5.fmi
- Upgrade to boost 1.41

* Wed Dec 30 2009 westerba <antti.westerberg@fmi.fi> - 9.12.30-1.el5.fmi
- New header parser implementation and fixed the compilation problems.

* Tue Jul 14 2009 mheiskan <mika.heiskanen@fmi.fi> - 9.7.14-2.el5.fmi
- Upgrade to boost 1.39

* Tue Jul 14 2009 westerba <antti.westerberg@fmi.fi> - 9.7.14-1.el5.fmi
- Improved Backend Server logic with shared_ptr's and graceful failures.

* Thu Jul  2 2009 westerba <antti.westerberg@fmi.fi> - 9.7.2-1.el5.fmi
- Remove the backend host from the service pool if forwarding fails.

* Fri Jun 26 2009 westerba <antti.westerberg@fmi.fi> - 9.6.26-1.el5.fmi
- Passing the X-Forwarded-For header to backends

* Thu Jun 11 2009 westerba <antti.westerberg@fmi.fi> - 9.6.11-1.el5.fmi
- Added the possibility to access a selected backend server

* Mon May 18 2009 westerba <antti.westerberg@fmi.fi> - 9.5.18-1.el5.fmi
- Fixed a critical bug which caused the server to crash if IP was not able to be resolved due to failing name server.

* Thu Feb 5 2009 westerba <antti.westerberg@fmi.fi> - 9.2.5-1.el5.fmi
- Fixed a bug that caused instable proxy operation if backend server crashed

* Fri Dec 12 2008 mheiskan <mika.heiskanen@fmi.fi> - 8.12.12-1.el5.fmi
- Error handling

* Wed Nov 26 2008 westerba <antti.westerberg@fmi.fi> - 8.11.26-1.el5.fmi
- Fixed the bug causing truncation of binary content

* Wed Nov 19 2008 westerba <antti.westerberg@fmi.fi> - 8.11.19-1.el5.fmi
- Compiled against new SmartMet API. Passes backend responses as-is.

* Mon Oct 6 2008 mheiskan <mika.heiskanen@fmi.fi> - 8.10.6-1.el5.fmi
- First release

