// capi/dial.go
// The dial, on its own goroutine.
//
// WHY THIS FILE EXISTS. omegassh_open used to dial inline: it connected,
// authenticated, opened a shell and only then returned a handle. That is one
// less moving part, and it cost three things that Phase 2 had already named
// and could not deliver.
//
// It cost the states. By the time there was a session to publish for, the
// connect and the login were over, so WrapSSH started at connected and the
// connection overlay had nothing to render but the end. A spinner needs
// something to spin on.
//
// It cost interactive authentication. A dial that blocks its caller cannot
// stop halfway to ask a question and wait for the answer, because the only
// thread that could answer is the one inside the call. Parking in
// authenticating and publishing a prompt needs the dial to be somewhere else.
//
// And it cost the GUI thread. "Call it from a worker, never from the GUI
// thread" is a rule that is followed until somebody forgets, and forgetting
// means an application frozen for the whole connect timeout against gear that
// is switched off.
//
// WHAT IT COSTS INSTEAD. A failed dial is no longer a return value. It is a
// state change on a handle the caller already holds, and the handle has to be
// closed either way -- see the contract note on omegassh_open in
// ../include/omegassh/omegassh.h. Everything that CAN be decided without the
// network is still decided synchronously, in prepare(), precisely so that the
// asynchronous half is only ever the network's answer and never a mistake the
// caller could have been told about at once.
//
// CANCELLATION. omegassh_close on a handle that is still dialing returns
// immediately: it retires the handle, and the dial finds st.closed when it
// lands and closes whatever it built instead of filing it. What it does not do
// is interrupt the dial in progress -- sshcore.Dial has no cancel, only a
// timeout -- so a socket and a log file can outlive their tab by up to
// timeout_seconds. That is an operator closing a tab and a goroutine tidying
// up behind them, which is the right trade for never blocking a close;
// interrupting properly means a context through the dial path and is worth
// doing when something needs it.
package main

import (
	"github.com/scottpeterman/omegassh/serialx"
	"github.com/scottpeterman/omegassh/sshcore"
	"github.com/scottpeterman/omegassh/telnetx"
	"github.com/scottpeterman/omegassh/transport"
)

// sshPlan is the SSH half of a prepared dial: a config with every reference
// checked, and the pty to ask for once there is a channel to ask on.
type sshPlan struct {
	cfg  sshcore.Config
	opts sshcore.ShellOptions
}

// dialPlan is everything prepare() worked out, ready to be acted on. Exactly
// one of the three transport fields is set; which one was settled by the
// configuration, which is why nothing below this point branches on it twice.
type dialPlan struct {
	kind    transport.Kind
	summary string

	// initial is the state the handle is filed in before the dial starts.
	// It is per-transport rather than uniform because the transports are
	// not uniform: serial has no connect phase to be in.
	initial transport.State

	// log is already open. A log that cannot be opened fails the whole call
	// synchronously, so by the time a plan exists this is either a working
	// file or deliberately nil.
	log *transport.Logger

	// size is the geometry from the configuration, pushed after a telnet
	// connect. SSH carries it in the pty request instead.
	size transport.Size

	ssh    *sshPlan
	telnet *telnetx.Backend
	serial *serialx.Backend
}

// discard releases what prepare() opened for a dial that will not happen.
func (p *dialPlan) discard() { p.log.Close() }

// connect performs the blocking half. publish is called as it passes each
// state worth reporting; it must not block, and it does not -- it queues an
// event and pokes a pipe.
func (p *dialPlan) connect(publish func(transport.State)) (transport.Session, error) {
	switch {
	case p.ssh != nil:
		cfg := p.ssh.cfg
		// The stages come from sshcore because only sshcore knows where the
		// dial actually is. Publishing authenticating on the way in would
		// describe a session stuck on an unanswered SYN as though it were
		// arguing about a password, which is the opposite of the help the
		// overlay is there to give.
		cfg.Progress = func(stage sshcore.DialStage) {
			switch stage {
			case sshcore.StageConnecting:
				publish(transport.StateConnecting)
			case sshcore.StageAuthenticating:
				publish(transport.StateAuthenticating)
			}
		}
		sess, err := sshcore.OpenShell(cfg, p.ssh.opts)
		if err != nil {
			return nil, err
		}
		return transport.WrapSSH(sess, p.summary, p.log), nil

	case p.telnet != nil:
		if err := p.telnet.Connect(); err != nil {
			return nil, err
		}
		// The size is pushed after Connect: NAWS is only sent once the peer
		// has agreed to it, and Resize records the geometry either way, so a
		// device that negotiates late still gets the real width.
		if p.size.Valid() {
			_ = p.telnet.Resize(p.size)
		}
		return transport.Wrap(p.telnet, transport.KindTelnet, p.summary, p.log), nil

	default:
		if err := p.serial.Connect(); err != nil {
			return nil, err
		}
		return transport.Wrap(p.serial, transport.KindSerial, p.summary, p.log), nil
	}
}

// runDial is the goroutine omegassh_open leaves behind. It owns the plan from
// here: whatever happens, the plan is either adopted by the handle or fully
// released, and nothing is left half-open.
func (st *handleState) runDial(p *dialPlan) {
	sess, err := p.connect(st.publish)
	if err != nil {
		p.discard()
		st.failDial(err)
		return
	}
	st.adopt(sess)
}

// adopt hands a live session to the handle and starts the read loop.
//
// The handoff runs under st.mu, and installing the event handler and starting
// the pump are inside it rather than after it. That is deliberate: the only
// other writer here is omegassh_close, and if these ran outside the lock a
// close arriving mid-handoff could tear the notifier down between the handler
// being installed and the pump being started -- a wake landing on a closed
// descriptor, which is exactly what the notifyMu discipline elsewhere exists
// to prevent. Under the lock there are two outcomes and both are whole: the
// session is adopted, or it is closed and never filed.
func (st *handleState) adopt(sess transport.Session) {
	st.mu.Lock()
	if st.closed {
		st.mu.Unlock()
		// The tab was closed while this was dialing. Nothing is published:
		// there is nobody left to publish to, and the handle is already gone
		// from the registry.
		sess.Close()
		return
	}
	st.sess = sess
	pending := st.resize
	sess.SetEventHandler(st.onEvent)
	go st.pump()
	st.mu.Unlock()

	if pending.Valid() {
		_ = sess.Resize(pending)
	}

	// sess.State(), not StateConnected. Almost always they are the same
	// thing, and when they are not it is because the session ended between
	// being built and the handler being installed -- a device that drops the
	// connection the instant the shell starts -- and its own state change was
	// published to nobody. Reading it back is what stops the handle sitting
	// on "connecting" for a session that is already over. publish refuses to
	// override a terminal state, so the race the other way is settled too.
	st.publish(sess.State())
}
