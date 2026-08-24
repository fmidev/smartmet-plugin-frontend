#include "BackendConnectionPool.h"
#include <macgyver/Exception.h>
#include <sys/socket.h>
#include <algorithm>
#include <cerrno>
#include <utility>

namespace SmartMet
{
namespace
{
// ----------------------------------------------------------------------
/*!
 * \brief Is this idle connection still usable?
 *
 * A peek that would block is the only good answer: it means the connection is
 * open and the backend has sent nothing on it.
 *
 * Zero bytes means the backend has closed its end, which is the expected fate
 * of a connection left idle past the backend's keep-alive timeout. Readable
 * bytes are worse: they are the tail of a response we thought we had finished
 * reading, so the connection is out of sync and everything on it after this
 * point would be misattributed. Both are discarded.
 *
 * This closes the window, it does not eliminate it - the backend can close
 * between the peek and the write. That is what the caller's one-shot retry on a
 * fresh connection is for.
 */
// ----------------------------------------------------------------------

bool isStillUsable(int theFd)
{
  char probe = 0;
  const ssize_t bytes = ::recv(theFd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);

  if (bytes > 0)
    return false;  // Unexpected data: the connection is out of sync
  if (bytes == 0)
    return false;  // The backend has closed its end

  return (errno == EAGAIN || errno == EWOULDBLOCK);
}

}  // namespace

BackendConnectionPool::BackendConnectionPool(bool theEnabled,
                                             std::size_t theMaxIdlePerBackend,
                                             int theIdleTimeoutSeconds)
    : itsEnabled(theEnabled),
      itsMaxIdlePerBackend(theMaxIdlePerBackend),
      itsIdleTimeout(std::max(1, theIdleTimeoutSeconds))
{
}

void BackendConnectionPool::expireIdle(std::deque<Entry>& theEntries)
{
  const auto now = std::chrono::steady_clock::now();

  // Entries are pushed in idle order, so the oldest are at the front
  while (!theEntries.empty() && now - theEntries.front().idleSince > itsIdleTimeout)
  {
    boost::system::error_code ignored;
    theEntries.front().socket.close(ignored);
    theEntries.pop_front();
    ++itsExpired;
  }
}

void BackendConnectionPool::sweepIfDue()
{
  const auto now = std::chrono::steady_clock::now();
  if (now - itsLastSweep < itsIdleTimeout)
    return;

  itsLastSweep = now;

  for (auto pos = itsIdleConnections.begin(); pos != itsIdleConnections.end();)
  {
    expireIdle(pos->second);
    if (pos->second.empty())
      pos = itsIdleConnections.erase(pos);
    else
      ++pos;
  }
}

bool BackendConnectionPool::acquire(const std::string& theIP,
                                    unsigned short thePort,
                                    Socket& theSocket)
{
  try
  {
    if (!itsEnabled)
      return false;

    boost::unique_lock<boost::mutex> lock(itsMutex);
    sweepIfDue();

    auto pos = itsIdleConnections.find(Key{theIP, thePort});
    if (pos == itsIdleConnections.end())
      return false;

    expireIdle(pos->second);

    // Take the most recently used connection: it is the one least likely to
    // have been closed by the backend in the meantime, and leaving the older
    // ones to expire keeps the pool from holding more than the load needs.
    while (!pos->second.empty())
    {
      Entry entry = std::move(pos->second.back());
      pos->second.pop_back();

      if (entry.socket.is_open() && isStillUsable(entry.socket.native_handle()))
      {
        theSocket = std::move(entry.socket);
        ++itsHits;
        return true;
      }

      boost::system::error_code ignored;
      entry.socket.close(ignored);
      ++itsDiscarded;
    }

    return false;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

void BackendConnectionPool::release(const std::string& theIP,
                                    unsigned short thePort,
                                    Socket&& theSocket)
{
  try
  {
    boost::system::error_code ignored;

    if (!itsEnabled || !theSocket.is_open())
    {
      theSocket.close(ignored);
      return;
    }

    boost::unique_lock<boost::mutex> lock(itsMutex);
    sweepIfDue();

    auto& entries = itsIdleConnections[Key{theIP, thePort}];
    expireIdle(entries);

    if (entries.size() >= itsMaxIdlePerBackend)
    {
      // More idle connections than this backend is expected to need. Keeping
      // them would only tie up file descriptors on both ends.
      theSocket.close(ignored);
      ++itsRejected;
      return;
    }

    entries.push_back(Entry{std::move(theSocket), std::chrono::steady_clock::now()});
    ++itsReturned;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

void BackendConnectionPool::closeBackend(const std::string& theIP, unsigned short thePort)
{
  try
  {
    boost::unique_lock<boost::mutex> lock(itsMutex);

    auto pos = itsIdleConnections.find(Key{theIP, thePort});
    if (pos == itsIdleConnections.end())
      return;

    for (auto& entry : pos->second)
    {
      boost::system::error_code ignored;
      entry.socket.close(ignored);
    }

    itsIdleConnections.erase(pos);
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

void BackendConnectionPool::closeAll()
{
  try
  {
    boost::unique_lock<boost::mutex> lock(itsMutex);

    for (auto& backend : itsIdleConnections)
      for (auto& entry : backend.second)
      {
        boost::system::error_code ignored;
        entry.socket.close(ignored);
      }

    itsIdleConnections.clear();
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

BackendConnectionPool::Statistics BackendConnectionPool::getStatistics() const
{
  try
  {
    Statistics stats;
    stats.hits = itsHits;
    stats.misses = itsMisses;
    stats.returned = itsReturned;
    stats.discarded = itsDiscarded;
    stats.expired = itsExpired;
    stats.replayed = itsReplayed;
    stats.rejected = itsRejected;

    boost::unique_lock<boost::mutex> lock(itsMutex);
    for (const auto& backend : itsIdleConnections)
      stats.backends.push_back(
          BackendStatistics{backend.first.ip, backend.first.port, backend.second.size()});

    return stats;
  }
  catch (...)
  {
    throw Fmi::Exception::Trace(BCP, "Operation failed!");
  }
}

}  // namespace SmartMet
