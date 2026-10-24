#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise goal-set scheduling and independent timers through the real owner."""

import os
import re
import signal
import subprocess
import threading
import time
import unittest

import test_goal_wait_channel as fixture


def request_text(value):
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return '\n'.join(request_text(item) for item in value)
    if isinstance(value, dict):
        return '\n'.join(request_text(item) for item in value.values())
    return ''


class MultipleControllersTests(unittest.TestCase):
    setUp = fixture.WaitChannelTests.setUp
    close_children = fixture.WaitChannelTests.close_children
    start = fixture.WaitChannelTests.start
    send = fixture.WaitChannelTests.send
    events = fixture.WaitChannelTests.events

    def respond(self, handler, request, sequence):
        index = len(self.requests)
        self.requests.append(request)
        step = self.steps[index] if index < len(self.steps) else (None, 'EXTRA_TURN')
        if callable(step):
            step = step(request)
        name, arguments = step
        if name:
            body = self.provider.function_body(sequence, f'multiple_{sequence}', name, arguments)
        else:
            body = self.provider.response_body(sequence, arguments)
        self.provider.reply(handler, body.encode(), close_header=True)

    def goal_id(self, request, wording):
        text = request_text(request)
        found = re.findall(r'Persistent goal ([0-9a-f]{32}) is [^\n]+\n'
                           r'Parent: [^\n]*\nCurrent goal wording:\n' + re.escape(wording), text)
        self.assertTrue(found, text[-6000:])
        return found[-1]

    def update(self, wording, action):
        def step(request):
            arguments = {'id': self.goal_id(request, wording), 'action': action, 'text': None}
            if action == 'block':
                arguments.update(text='awaiting a fixture dependency', wait_for='operator')
            return 'update_goal', arguments
        return step

    def stop(self, child):
        self.send(child, '/exit', '--resume')
        child.wait(0)
        child.close()
        self.children.remove(child)

    def test_blocking_one_goal_keeps_other_work_running(self):
        self.steps = [
            ('create_goal', {'objective': 'first_multi'}),
            ('create_goal', {'objective': 'second_multi'}),
            self.update('first_multi', 'block'),
            (None, 'MULTI_CHECKPOINT'),
            self.update('second_multi', 'complete'),
            (None, 'MULTI_FINISHED'),
        ]
        child = self.start()
        self.send(child, 'Create the two explicitly requested fixture goals.', 'MULTI_FINISHED')
        child.until(b'READY>', 10)
        journal, events = self.events()
        starts = [event for event in events if event['type'] == 'goal_started']
        self.assertEqual(len(starts), 2)
        self.assertNotEqual(starts[0]['data']['goal_id'], starts[1]['data']['goal_id'])
        turns = [event for event in events if event['type'] == 'turn_started']
        self.assertEqual(len(turns), 2)
        self.assertEqual(turns[-1]['data']['input_kind'], 'goal')
        text = request_text(self.requests[4])
        self.assertIn(starts[0]['data']['goal_id'] + ' is blocked', text)
        self.assertIn(starts[1]['data']['goal_id'] + ' is active', text)
        self.send(child, '/goal', 'second_multi')
        self.stop(child)
        resumed = self.start('--resume', journal.parent.name)
        output = self.send(resumed, '/goal', 'second_multi')
        self.assertIn(starts[0]['data']['goal_id'].encode(), output)
        self.assertIn(b'blocked', output)
        self.assertIn(b'completed', output)
        self.stop(resumed)

    def test_continuation_can_start_after_existing_goal_blocks(self):
        self.steps = [
            ('create_goal', {'objective': 'blocked_outcome'}),
            ('update_goal', {'action': 'block', 'text': 'awaiting a fixture dependency',
                             'wait_for': 'operator'}),
            (None, 'OUTCOME_BLOCKED'),
            ('create_goal', {'objective': 'accepted_continuation'}),
            ('list_goals', {}),
            (None, 'CONTINUATION_CHECKPOINT'),
            self.update('accepted_continuation', 'complete'),
            (None, 'CONTINUATION_FINISHED'),
        ]
        child = self.start()
        self.send(child, 'Start the requested outcome and block on its dependency.',
                  'OUTCOME_BLOCKED')
        child.until(b'READY>', 10)
        journal, events = self.events()
        blocked, = [event for event in events if event['type'] == 'goal_blocked']
        self.stop(child)

        resumed = self.start('--resume', journal.parent.name)
        self.send(resumed, 'Create the separate, explicitly requested continuation goal.',
                  'CONTINUATION_CHECKPOINT')
        _, events = self.events()
        starts = [event for event in events if event['type'] == 'goal_started']
        self.assertEqual(len(starts), 2)
        self.assertEqual(blocked['data']['goal_id'], starts[0]['data']['goal_id'])
        self.assertLess(blocked['seq'], starts[1]['seq'])
        self.assertNotEqual(starts[0]['data']['goal_id'], starts[1]['data']['goal_id'])
        text = request_text(self.requests[5])
        self.assertIn(starts[0]['data']['goal_id'] + ' is blocked', text)
        self.assertIn(starts[1]['data']['goal_id'] + ' is active', text)
        self.assertIn('id=' + starts[0]['data']['goal_id'] + ' status=blocked', text)
        self.assertIn('id=' + starts[1]['data']['goal_id'] + ' status=active', text)
        resumed.until(b'CONTINUATION_FINISHED', 10)
        resumed.until(b'READY>', 10)
        _, events = self.events()
        completed, = [event for event in events if event['type'] == 'goal_completed']
        self.assertEqual(completed['data']['goal_id'], starts[1]['data']['goal_id'])
        turns = [event for event in events if event['type'] == 'turn_started']
        self.assertEqual(turns[-1]['data']['input_kind'], 'goal')
        self.stop(resumed)

        replayed = self.start('--resume', journal.parent.name)
        output = self.send(replayed, '/goal', 'accepted_continuation')
        self.assertIn(starts[0]['data']['goal_id'].encode(), output)
        self.assertIn(starts[1]['data']['goal_id'].encode(), output)
        self.assertIn(b'blocked', output)
        self.assertIn(b'completed', output)
        self.assertIn(b'wait channel: operator', output)
        self.stop(replayed)

    def test_pause_and_operator_resume_apply_to_the_whole_goal_set(self):
        self.steps = [
            ('create_goal', {'objective': 'first_multi'}),
            ('create_goal', {'objective': 'second_multi'}),
            self.update('first_multi', 'block'),
            self.update('second_multi', 'block'),
            (None, 'BOTH_BLOCKED'),
        ]
        child = self.start()
        self.send(child, 'Create two fixture goals and block both.', 'BOTH_BLOCKED')
        output = self.send(child, '/goal pause   ', 'second_multi')
        self.assertEqual(output.count(b': paused'), 2)
        self.steps += [self.update('first_multi', 'complete'),
                       self.update('second_multi', 'complete'), (None, 'RESUME_FINISHED')]
        self.send(child, '/goal resume', 'RESUME_FINISHED')
        text = request_text(self.requests[5])
        self.assertIn('explicit operator direction', text)
        self.assertIn('first_multi', text)
        self.assertIn('second_multi', text)
        _, events = self.events()
        self.assertEqual(sum(event['type'] == 'goal_paused' for event in events), 2)
        self.assertEqual(sum(event['type'] == 'goal_resumed' for event in events), 2)
        self.stop(child)

    def test_two_one_shot_timers_deliver_two_distinct_durable_inputs(self):
        self.steps = [
            ('timer', {'delay_ms': 150, 'text': 'WAKE_FIRST'}),
            ('timer', {'delay_ms': 250, 'text': 'WAKE_SECOND'}),
            (None, 'TIMERS_ARMED'), (None, 'FIRST_DELIVERED'), (None, 'SECOND_DELIVERED'),
        ]
        child = self.start()
        self.send(child, 'Schedule two independent reminders.', 'SECOND_DELIVERED')
        child.until(b'READY>', 10)
        _, events = self.events()
        scheduled = [event['data']['timer_id'] for event in events
                     if event['type'] == 'timer_scheduled']
        delivered = [event['data'] for event in events if event['type'] == 'input_received'
                     and event['data'].get('origin') == 'timer']
        self.assertEqual(len(scheduled), 2)
        self.assertEqual([receipt['timer_id'] for receipt in delivered], scheduled)
        self.assertEqual([receipt['text'] for receipt in delivered], ['WAKE_FIRST', 'WAKE_SECOND'])
        self.assertTrue(all(receipt['next_due_ms'] == 0 for receipt in delivered))
        self.assertFalse(any(event['type'] == 'timer_cancelled' for event in events))
        self.stop(child)

    def test_recurring_timer_coalesces_missed_intervals_and_can_be_cancelled(self):
        def delayed_response(request):
            self.assertIn('RECURRING_WAKE', request_text(request))
            time.sleep(.3)
            return None, 'RECURRING_FIRST'

        def cancel(request):
            ids = re.findall(r'Armed model timer ([0-9a-f]{32}):', request_text(request))
            self.assertTrue(ids)
            return 'timer', {'action': 'cancel', 'id': ids[-1]}

        self.steps = [
            ('timer', {'delay_ms': 80, 'text': 'RECURRING_WAKE', 'recurring': True}),
            (None, 'RECURRING_ARMED'), delayed_response, cancel,
            (None, 'RECURRING_CANCELLED'),
        ]
        child = self.start()
        self.send(child, 'Schedule the requested recurring reminder.', 'RECURRING_CANCELLED')
        child.until(b'READY>', 10)
        time.sleep(.2)
        _, events = self.events()
        scheduled, = [event['data'] for event in events if event['type'] == 'timer_scheduled']
        self.assertEqual(scheduled['interval_ms'], 80)
        delivered = [event['data'] for event in events if event['type'] == 'input_received'
                     and event['data'].get('origin') == 'timer']
        self.assertEqual(len(delivered), 2)
        self.assertTrue(all(item['timer_id'] == scheduled['timer_id'] for item in delivered))
        for item in delivered:
            self.assertGreater(item['next_due_ms'], item['received_at_ms'])
            self.assertEqual((item['next_due_ms'] - scheduled['due_ms']) % 80, 0)
        self.stop(child)

    def test_timers_survive_resume_and_cancel_only_the_target(self):
        def cancel_first(request):
            ids = re.findall(r'Armed model timer ([0-9a-f]{32}):', request_text(request))
            self.assertEqual(len(set(ids)), 2)
            return 'timer', {'action': 'cancel', 'id': ids[0]}

        self.steps = [('timer', {'delay_ms': 60000, 'text': 'LATER_FIRST'}),
                      ('timer', {'delay_ms': 60000, 'text': 'LATER_SECOND', 'recurring': True}),
                      (None, 'BEFORE_RESTART')]
        child = self.start()
        self.send(child, 'Schedule the two durable fixture reminders.', 'BEFORE_RESTART')
        child.until(b'READY>', 10)
        journal, _ = self.events()
        self.stop(child)
        self.steps += [cancel_first, ('timer', {'action': 'list'}), (None, 'AFTER_RESTART')]
        resumed = self.start('--resume', journal.parent.name)
        self.send(resumed, 'Cancel the first timer and list what remains.', 'AFTER_RESTART')
        text = request_text(self.requests[-1])
        self.assertIn('Armed timers: 1', text)
        self.assertIn('recurring=true', text)
        self.assertIn('LATER_SECOND', text)
        self.stop(resumed)

    def test_ambiguous_and_invalid_targeting_leave_other_goals_unchanged(self):
        self.steps = [('create_goal', {'objective': 'first_multi'}),
                      ('create_goal', {'objective': 'second_multi'}),
                      ('update_goal', {'action': 'complete'}),
                      self.update('first_multi', 'block'),
                      self.update('second_multi', 'block'), (None, 'TARGETS_READY')]
        child = self.start()
        self.send(child, 'Create the two fixture goals.', 'TARGETS_READY')
        self.assertIn('specify an unfinished goal ID', request_text(self.requests[3]))
        _, events = self.events()
        ids = [event['data']['goal_id'] for event in events if event['type'] == 'goal_started']
        self.send(child, '/goal set ' + 'f' * 32 + ' invalid', 'unfinished goal ID')
        self.send(child, '/goal clear', 'unfinished goal ID')
        self.send(child, '/goal status   ', 'second_multi')
        self.send(child, '/goal lock ' + ids[0] + '   ', 'wording locked')
        self.steps += [('update_goal', {'id': ids[0], 'action': 'rewrite', 'text': 'forbidden'}),
                       ('update_goal', {'id': ids[1], 'action': 'rewrite', 'text': 'rewritten'}),
                       (None, 'TARGETS_CHANGED')]
        self.send(child, 'Try both rewrites without resuming either goal.', 'TARGETS_CHANGED')
        self.assertIn('locked by the operator', request_text(self.requests[7]))
        _, events = self.events()
        replaced, = [event['data'] for event in events if event['type'] == 'goal_replaced']
        self.assertEqual(replaced['goal_id'], ids[1])
        self.assertFalse(any(event['type'] == 'goal_completed' for event in events))
        self.stop(child)

    def test_invalid_timer_arguments_do_not_create_or_cancel_schedules(self):
        bad = [{'action': 7, 'delay_ms': 100, 'text': 'bad'},
               {'id': 7, 'delay_ms': 100, 'text': 'bad'},
               {'recurring': 'yes', 'delay_ms': 100, 'text': 'bad'},
               {'id': 'f' * 32, 'action': 'cancel'},
               {'delay_ms': -1, 'text': 'bad'}]
        self.steps = [('timer', value) for value in bad] + [(None, 'INVALID_TIMERS_DONE')]
        child = self.start()
        self.send(child, 'Exercise the invalid timer requests.', 'INVALID_TIMERS_DONE')
        _, events = self.events()
        self.assertFalse(any(event['type'].startswith('timer_') for event in events))
        for request in self.requests[1:]:
            self.assertIn('failed', request_text(request))
        self.stop(child)

    def test_schedule_without_delay_does_not_cancel_an_armed_timer(self):
        self.steps = [('timer', {'delay_ms': 60000, 'text': 'KEEP_ARMED'}),
                      ('timer', {}), ('timer', {'delay_ms': None}),
                      ('timer', {'action': 'list'}), (None, 'MISSING_DELAY_DONE')]
        child = self.start()
        self.send(child, 'Keep the timer armed across invalid scheduling calls.',
                  'MISSING_DELAY_DONE')
        _, events = self.events()
        self.assertEqual(sum(event['type'] == 'timer_scheduled' for event in events), 1)
        self.assertFalse(any(event['type'] == 'timer_cancelled' for event in events))
        self.assertIn('Armed timers: 1', request_text(self.requests[-1]))
        self.stop(child)

    def test_lowered_wording_limit_preserves_saved_objectives(self):
        config = self.state / 'config.ini'
        config.write_text(config.read_text().replace('[agent]\n',
            '[agent]\nmax_goal_prompt_bytes=8192\n', 1))
        wording = 'saved_' + 'x' * 5000
        self.steps = [self.update(wording, 'block'), (None, 'LARGE_SAVED')]
        child = self.start()
        self.send(child, '/goal ' + wording, 'LARGE_SAVED')
        journal, _ = self.events()
        self.stop(child)
        config.write_text(config.read_text().replace('max_goal_prompt_bytes=8192',
                                                     'max_goal_prompt_bytes=4096'))
        resumed = self.start('--resume', journal.parent.name)
        self.send(resumed, '/goal', 'saved_')
        self.steps += [(None, 'SAVED_WORDING_VISIBLE')]
        self.send(resumed, '/ro Inspect the retained goal without changing it.',
                  'SAVED_WORDING_VISIBLE')
        self.assertIn(wording, request_text(self.requests[-1]))
        self.assertIn('controller suspended', request_text(self.requests[-1]))
        self.stop(resumed)

    def test_restart_after_timer_admission_keeps_one_wakeup(self):
        admitted = threading.Event()

        def interrupted(request):
            self.assertIn('CRASH_WAKE', request_text(request))
            admitted.set()
            time.sleep(.6)
            return None, 'OLD_OWNER_RESPONSE'

        def cancel(request):
            ids = re.findall(r'Armed model timer ([0-9a-f]{32}):', request_text(request))
            self.assertTrue(ids)
            return 'timer', {'action': 'cancel', 'id': ids[-1]}

        self.steps = [('timer', {'delay_ms': 1000, 'text': 'CRASH_WAKE', 'recurring': True}),
                      (None, 'CRASH_ARMED'), interrupted, cancel, (None, 'CRASH_RECOVERED')]
        child = self.start()
        self.send(child, 'Schedule the crash-recovery fixture reminder.', 'CRASH_ARMED')
        self.assertTrue(admitted.wait(5))
        journal, events = self.events()
        received = [event for event in events if event['type'] == 'input_received'
                    and event['data'].get('origin') == 'timer']
        self.assertEqual(len(received), 1)
        rows = subprocess.check_output(['ps', '-axww', '-o', 'pid=', '-o', 'command='], text=True)
        for row in rows.splitlines():
            if '--dotdir ' + str(self.state) + ' ' in row:
                try:
                    os.kill(int(row.split(None, 1)[0]), signal.SIGKILL)
                except ProcessLookupError:
                    pass
        child.close()
        self.children.remove(child)
        resumed = self.start('--resume', journal.parent.name)
        resumed.until(b'CRASH_RECOVERED', 10)
        _, events = self.events()
        self.assertEqual(sum(event['type'] == 'input_received' and
                             event['data'].get('origin') == 'timer' for event in events), 1)
        self.assertNotIn('OLD_OWNER_RESPONSE', request_text(self.requests[-1]))
        self.stop(resumed)

    def test_default_wording_limit_counts_utf8_bytes(self):
        child = self.start()
        output = self.send(child, '/goal ' + 'x' * 4097, '4096 UTF-8 bytes')
        self.assertIn(b'4096 UTF-8 bytes', output)
        self.steps = [self.update('é' * 2048, 'block'), (None, 'LIMIT_ACCEPTED')]
        self.send(child, '/goal ' + 'é' * 2048, 'LIMIT_ACCEPTED')
        _, events = self.events()
        started, = [event for event in events if event['type'] == 'goal_started']
        self.assertEqual(len(started['data']['prompt'].encode()), 4096)
        self.stop(child)


if __name__ == '__main__':
    unittest.main()
