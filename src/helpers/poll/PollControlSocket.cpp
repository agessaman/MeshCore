#include "PollBuild.h"

#if defined(WITH_REPEATER_POLL)
#include "PollControlSocket.h"
#include <errno.h>
#include <lwip/sockets.h>

int pollControlSend(int fd, const uint8_t* data, size_t len) {
  const int n = lwip_send(fd, data, len, MSG_DONTWAIT);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
  return n;
}
#endif
