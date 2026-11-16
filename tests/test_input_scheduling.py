#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Enter steers active work; cancellation advances armed user input without spinning."""

import json
import os
import socket
import threading
import time
import unittest

import test_goal_wait_channel as fixture


class InputSchedulingTests(unittest.TestCase):
    setUp = fixture.WaitChannelTests.setUp
    close_children = fixture.WaitChannelTests.close_children
    start = fixture.WaitChannelTests.start
    send = fixture.WaitChannelTests.send
    events = fixture.WaitChannelTests.events

    def respond(self, handler, request, sequence):
        self.requests.append(request)
        if len(self.requests) == 1 and 'change course now' not in json.dumps(request):
            self.started.set()
            self.hold_release.wait(15)
        try:
            body = self.provider.response_body(sequence, 'INPUT_SCHEDULING_DONE')
            self.provider.reply(handler, body.encode(), close_header=True)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def held_turn(self, goal=False):
        self.started = threading.Event()
        self.hold_release = threading.Event()
        child = self.start()
        self.send(child, '/goal held input work' if goal else 'held input work', 'BUSY>')
        self.assertTrue(self.started.wait(5))
        return child

    def wait_event(self, kind):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            _, events = self.events()
            matches = [event for event in events if event['type'] == kind]
            if matches:
                return matches[-1]
            time.sleep(.02)
        self.fail('missing event: ' + kind)

    def finish(self, child):
        self.hold_release.set()
        self.send(child, '/exit', '--resume')
        child.wait(0)

    def check_cancel_dispatch(self, goal):
        child = self.held_turn(goal=goal)
        self.send(child, '/queue explicit next task', 'queued (/next or /q c)')
        self.wait_event('future_turn_queued')
        child.output.clear()
        os.write(child.master, b'\x03')
        child.until(b'INPUT_SCHEDULING_DONE', 5)
        child.until(b'READY>', 5)
        self.send(child, '/q', 'future-turn queue is empty')
        _, events = self.events()
        turns = [event for event in events if event['type'] == 'turn_started']
        self.assertEqual([event['data']['input_kind'] for event in turns],
                         ['goal' if goal else 'direct', 'queued'])
        if goal:
            paused = self.wait_event('goal_paused')
            self.assertLess(events.index(paused), events.index(turns[-1]))
        self.assertEqual(turns[-1]['data']['text'], 'explicit next task')
        self.assertFalse(any(event['type'] == 'goal_resumed' for event in events))
        self.finish(child)

    def test_cancel_dispatches_armed_queue_and_keeps_commands_responsive(self):
        self.check_cancel_dispatch(goal=True)

    def test_cancel_direct_turn_preserves_armed_user_queue(self):
        self.check_cancel_dispatch(goal=False)

    def test_cleared_queue_is_not_dispatched_after_cancellation(self):
        child = self.held_turn(goal=True)
        self.send(child, '/queue removed user task', 'queued (/next or /q c)')
        self.send(child, '/q c', 'future turn cancelled')
        os.write(child.master, b'\x03')
        self.wait_event('turn_interrupted')
        self.send(child, '/q', 'future-turn queue is empty')
        self.assertEqual(len(self.requests), 1)
        self.finish(child)

    def test_paused_retained_goal_keeps_queue_inspection_responsive(self):
        child = self.held_turn(goal=True)
        self.send(child, '/queue retained user task', 'queued (/next or /q c)')
        self.send(child, '/goal pause', 'Goal paused')
        self.wait_event('goal_paused')
        self.send(child, '/q', 'retained user task')
        self.send(child, '/q c', 'future turn cancelled')
        self.finish(child)

    def test_enter_typeahead_steers_instead_of_using_stale_idle_snapshot(self):
        self.started = threading.Event()
        self.hold_release = threading.Event()
        child = self.start()
        # Both submissions can be captured by the idle composer before the
        # owner starts the first turn. Enter still addresses the active turn.
        os.write(child.master, b'held input work\rchange course now\r')
        self.wait_event('steering_added')
        child.until(b'INPUT_SCHEDULING_DONE', 5)
        _, events = self.events()
        self.assertFalse(any(event['type'] == 'future_turn_queued' for event in events))
        steering = [event for event in events if event['type'] == 'steering_added']
        self.assertEqual([event['data']['text'] for event in steering], ['change course now'])
        self.assertIn('change course now', json.dumps(self.requests[-1]))
        self.finish(child)

    def test_server_start_preserves_enter_steering(self):
        child = self.held_turn()
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
        self.send(child, f'/server start localhost:{port}', 'hosting started')
        self.send(child, 'change course now', 'INPUT_SCHEDULING_DONE')
        _, events = self.events()
        self.assertFalse(any(event['type'] == 'future_turn_queued' for event in events))
        self.wait_event('steering_added')
        self.send(child, '/server stop', 'hosting stopped')
        self.finish(child)

    def test_idle_queue_remains_unarmed_until_next(self):
        self.started = threading.Event()
        self.hold_release = threading.Event()
        self.hold_release.set()
        child = self.start()
        self.send(child, '/queue deliberate idle task', 'queued (/next or /q c)')
        os.write(child.master, b'\x03')
        self.send(child, '/q', 'deliberate idle task')
        self.assertEqual(self.requests, [])
        self.send(child, '/next', 'INPUT_SCHEDULING_DONE')
        self.send(child, '/q', 'future-turn queue is empty')
        self.finish(child)


if __name__ == '__main__':
    unittest.main()
