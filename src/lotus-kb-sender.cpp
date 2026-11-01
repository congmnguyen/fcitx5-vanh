/*
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */
#include "lotus-kb-sender.h"

#include "lotus-log.h"
#include "lotus-utils.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <utility>

KbSender::KbSender(std::string socketPath) : socketPath_(std::move(socketPath)) {}

KbSender::~KbSender() {
    close();
}

bool KbSender::connected() const {
    std::scoped_lock lock(mutex_);
    return fd_.isValid();
}

bool KbSender::ensureConnected() {
    std::scoped_lock lock(mutex_);
    return fd_.isValid() || connectLocked();
}

bool KbSender::send(const KbMsg& msg) {
    std::scoped_lock lock(mutex_);
    if (!fd_.isValid() && !connectLocked()) {
        LOTUS_ERROR("Cannot send key request since cannot connect to uinput server");
        return false;
    }
    const ssize_t written = ::send(fd_.get(), &msg, sizeof(msg), MSG_NOSIGNAL);
    if (std::cmp_equal(written, sizeof(msg))) {
        return true;
    }
    LOTUS_WARN("Failed to send key request: " << std::strerror(errno));
    fd_.reset();
    if (!connectLocked()) {
        return false;
    }
    const ssize_t retried = ::send(fd_.get(), &msg, sizeof(msg), MSG_NOSIGNAL);
    if (std::cmp_equal(retried, sizeof(msg))) {
        LOTUS_INFO("Reconnected to uinput server successfully");
        return true;
    }
    LOTUS_WARN("Failed to send key request after reconnect: " << std::strerror(errno));
    return false;
}

void KbSender::close() {
    std::scoped_lock lock(mutex_);
    fd_.reset();
}

bool KbSender::connectLocked() {
    // Refuses a socket whose owner is not the uinput server user (verifyServerPeer()).
    FdGuard candidate(connectServerSocket(socketPath_, false, "keyboard socket"));
    if (!candidate.isValid()) {
        return false;
    }
    fd_ = std::move(candidate);
    return true;
}
