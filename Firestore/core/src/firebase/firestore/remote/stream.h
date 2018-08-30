/*
 * Copyright 2018 Google
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_REMOTE_STREAM_H_
#define FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_REMOTE_STREAM_H_

#include <memory>
#include <string>

#include "Firestore/core/src/firebase/firestore/auth/credentials_provider.h"
#include "Firestore/core/src/firebase/firestore/auth/token.h"
#include "Firestore/core/src/firebase/firestore/remote/datastore.h"
#include "Firestore/core/src/firebase/firestore/remote/exponential_backoff.h"
#include "Firestore/core/src/firebase/firestore/remote/grpc_operation.h"
#include "Firestore/core/src/firebase/firestore/remote/grpc_stream.h"
#include "Firestore/core/src/firebase/firestore/remote/stream_objc_bridge.h"
#include "Firestore/core/src/firebase/firestore/util/async_queue.h"
#include "Firestore/core/src/firebase/firestore/util/status.h"
#include "Firestore/core/src/firebase/firestore/util/statusor.h"
#include "absl/strings/string_view.h"
#include "grpcpp/support/byte_buffer.h"

namespace firebase {
namespace firestore {
namespace remote {

/**
 * A `Stream` is an abstract base class that represents a bidirectional
 * streaming connection to the Firestore backend. It's built on top of gRPC C++
 * library and adds several critical features for our clients:
 *
 *   - Exponential backoff on failure (independent of the gRPC mechanism)
 *   - Authentication via CredentialsProvider
 *   - Dispatching all callbacks into the shared Firestore async queue
 *   - Closing idle streams after 60 seconds of inactivity
 *
 * Subclasses of `Stream`:
 *
 *   - Implement serialization and deserialization of protocol buffers
 *   - Notify their delegate about stream open/read/error events
 *   - Create and finish the underlying gRPC streams.
 *
 * ## Starting and Stopping
 *
 * Streams are stateful and need to be `Start`ed before messages can
 * be sent and received. A `Stream` can be started and stopped repeatedly.
 */
class Stream : public GrpcStreamObserver,
               public std::enable_shared_from_this<Stream> {
 public:
  /**
   * `Stream` can be in one of 5 states (each described in detail below)
   * shown in the following state transition diagram:
   *
   *          Start() called             auth & connection succeeded
   * INITIAL ----------------> STARTING -----------------------------> OPEN
   *                             ^  |                                   |
   *                             |  |                    error occurred |
   *                             |  \-----------------------------v-----/
   *                             |                                |
   *                    backoff  |                                |
   *                    elapsed  |              Start() called    |
   *                             \--- BACKOFF <---------------- ERROR
   *
   * [any state] --------------------------> INITIAL
   *               Stop() called or
   *               idle timer expired
   */
  enum class State {
    /**
     * The stream is not yet running and there's no error condition.
     * Calling `Start` will start the stream immediately without backoff.
     * While in this state, `IsStarted` will return false.
     */
    Initial,

    /**
     * The stream is starting, either waiting for an auth token or for the
     * stream to successfully open. While in this state, `IsStarted` will
     * return true but `IsOpen` will return false.
     */
    Starting,

    /**
     * The stream is up and running. Requests and responses can flow
     * freely. Both `IsStarted` and `IsOpen` will return true.
     */
    Open,

    /**
     * The stream encountered an error. The next start attempt will back off.
     * While in this state, `IsStarted` will return false.
     */
    Error,

    /**
     * An in-between state after an error where the stream is waiting before
     * re-starting. After waiting is complete, the stream will try to open.
     * While in this state, `IsStarted` will return true but `IsOpen` will
     * return false.
     */
    Backoff
  };

  Stream(util::AsyncQueue* async_queue,
         auth::CredentialsProvider* credentials_provider,
         Datastore* datastore,
         util::TimerId backoff_timer_id,
         util::TimerId idle_timer_id);

  /**
   * Starts the stream. Only allowed if `IsStarted` returns false. The stream is
   * not immediately ready for use: `OnStreamStart` will be invoked when the
   * stream is ready for outbound requests, at which point `IsOpen` will return
   * true.
   *
   * When start returns, `IsStarted` will return true.
   */
  void Start();

  /**
   * Stops the stream. This call is idempotent and allowed regardless of the
   * current `IsStarted` state.
   *
   * When stop returns, `IsStarted` and `IsOpen` will both return false.
   */
  void Stop();

  /**
   * Returns true if `Start` has been called and no error has occurred. True
   * indicates the stream is open or in the process of opening (which
   * encompasses respecting backoff, getting auth tokens, and starting the
   * actual stream). Use `IsOpen` to determine if the stream is open and ready
   * for outbound requests.
   */
  bool IsStarted() const;

  /**
   * Returns true if the underlying stream is open (`OnStreamStart` has been
   * called) and the stream is ready for outbound requests.
   */
  bool IsOpen() const;

  /**
   * After an error, the stream will usually back off on the next attempt to
   * start it. If the error warrants an immediate restart of the stream, the
   * sender can use this to indicate that the receiver should not back off.
   *
   * Each error will call `OnStreamClose`. That function can decide to
   * cancel backoff if required.
   */
  void CancelBackoff();

  /**
   * Marks this stream as idle. If no further actions are performed on the
   * stream for one minute, the stream will automatically close itself and
   * notify the stream's `OnClose` handler with Status::OK. The stream will then
   * be in a non-started state, requiring the caller to start the stream again
   * before further use.
   *
   * Only streams that are in state 'Open' can be marked idle, as all other
   * states imply pending network operations.
   */
  void MarkIdle();

  /**
   * Marks the stream as active again, preventing auto-closing of the stream.
   * Can be called from any state -- if the stream is not in state `Open`, this
   * is a no-op.
   */
  void CancelIdleCheck();

  // `GrpcStreamObserver` interface -- do not use.
  void OnStreamStart() override;
  void OnStreamRead(const grpc::ByteBuffer& message) override;
  void OnStreamError(const util::Status& status) override;

 protected:
  // `Stream` expects all its methods to be called on the worker queue.
  void EnsureOnQueue() const;
  void Write(grpc::ByteBuffer&& message);
  void ResetBackoff();
  std::string GetDebugDescription() const;

 private:
  // The interface for the derived classes.

  virtual std::unique_ptr<GrpcStream> CreateGrpcStream(
      Datastore* datastore, absl::string_view token) = 0;
  // PORTING NOTE: equivalent to `tearDown`.
  virtual void FinishGrpcStream(GrpcStream* stream) = 0;
  virtual void DoOnStreamStart() = 0;
  virtual util::Status DoOnStreamRead(const grpc::ByteBuffer& message) = 0;
  virtual void DoOnStreamFinish(const util::Status& status) = 0;
  // PORTING NOTE: C++ cannot rely on RTTI, unlike other platforms.
  virtual std::string GetDebugName() const = 0;

  // Used to prevent auth if the stream happens to be restarted before token is
  // received.
  int generation() const {
    return generation_;
  }

  void RaiseGeneration() {
    ++generation_;
  }

  void Authenticate();
  void ResumeStartAfterAuth(const util::StatusOr<auth::Token>& maybe_token);

  void BackoffAndTryRestarting();
  void ResumeStartFromBackoff();
  void StopDueToIdleness();

  void ResetGrpcStream();

  State state_ = State::Initial;

  std::unique_ptr<GrpcStream> grpc_stream_;

  auth::CredentialsProvider* credentials_provider_ = nullptr;
  util::AsyncQueue* worker_queue_ = nullptr;
  Datastore* datastore_ = nullptr;

  ExponentialBackoff backoff_;
  util::TimerId idle_timer_id_{};
  util::DelayedOperation idleness_timer_;

  // Generation is incremented in each call to `Stop`.
  int generation_ = 0;
};

}  // namespace remote
}  // namespace firestore
}  // namespace firebase

#endif  // FIRESTORE_CORE_SRC_FIREBASE_FIRESTORE_REMOTE_STREAM_H_