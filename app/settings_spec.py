"""The settings the start window offers, in plain words.

Each entry is one switch of the game server (the lines people otherwise edit in the iPhone release's
docker-compose.yml), of the game window, or of this launcher. `where` says which:
  'server'   an environment setting for the game server, kept in settings.json under "server_environment";
  'window'   a number in client/desktop.ini, shared with the bar of controls inside the game window;
  'launcher' a value in settings.json itself.
Kinds: 'toggle' (on/off values), 'choice' ((value, label) pairs), 'number' (low, high), 'text'.
A server setting left at its default is not written, so the server's own default keeps applying.
"""
from __future__ import annotations

DAYS = [('0', 'Monday'), ('1', 'Tuesday'), ('2', 'Wednesday'), ('3', 'Thursday'), ('4', 'Friday'), ('5', 'Saturday'), ('6', 'Sunday')]
HOURS = [(str(h), f'{(h - 1) % 12 + 1}:00 {"AM" if h < 12 else "PM"}') for h in range(24)]
ZONES = ['America/New_York', 'America/Chicago', 'America/Denver', 'America/Los_Angeles', 'America/Anchorage', 'Pacific/Honolulu',
         'America/Toronto', 'America/Vancouver', 'America/Mexico_City', 'America/Sao_Paulo', 'America/Argentina/Buenos_Aires',
         'Europe/London', 'Europe/Lisbon', 'Europe/Paris', 'Europe/Berlin', 'Europe/Madrid', 'Europe/Rome', 'Europe/Amsterdam',
         'Europe/Stockholm', 'Europe/Warsaw', 'Europe/Athens', 'Europe/Helsinki', 'Europe/Istanbul', 'Europe/Moscow',
         'Africa/Johannesburg', 'Asia/Dubai', 'Asia/Kolkata', 'Asia/Bangkok', 'Asia/Jakarta', 'Asia/Singapore', 'Asia/Manila',
         'Asia/Hong_Kong', 'Asia/Shanghai', 'Asia/Taipei', 'Asia/Seoul', 'Asia/Tokyo', 'Australia/Perth', 'Australia/Sydney',
         'Pacific/Auckland', 'UTC']


def _t(key, label, text, default='1', on='1', off='0', where='server'):
    return {'key': key, 'label': label, 'help': text, 'kind': 'toggle', 'default': default, 'on': on, 'off': off, 'where': where}


def _n(key, label, text, default, low, high, where='server'):
    return {'key': key, 'label': label, 'help': text, 'kind': 'number', 'default': str(default), 'low': low, 'high': high, 'where': where}


def _c(key, label, text, default, choices, where='server'):
    return {'key': key, 'label': label, 'help': text, 'kind': 'choice', 'default': default, 'choices': choices, 'where': where}


def _x(key, label, text, default='', where='server'):
    return {'key': key, 'label': label, 'help': text, 'kind': 'text', 'default': default, 'where': where}


GROUPS = [
    ('Game window', 'How the game looks and sounds on this computer. Most of these can also be changed while playing, '
                    'from the bar above the game.', [
        _c('size', 'Window size', 'How much of the screen the game window takes when it opens.', '2',
           [('0', 'Small'), ('1', 'Medium'), ('2', 'Large')], 'window'),
        _t('fullscreen', 'Start in full screen', 'F11 switches between full screen and a window while playing.', '0', where='window'),
        _t('render_screen', 'Sharpest picture in full screen',
           'The game draws its picture at the full height of your screen instead of the large window\'s height. '
           'Slightly more work for the graphics card.', '0', where='window'),
        _t('smooth', 'Smooth picture', 'Smooths the picture when the window is not at its natural size. Off shows hard pixels.', '1', where='window'),
        _t('vsync', 'Match the screen\'s refresh',
           'On a 60 Hz screen each frame waits for the screen instead of the game\'s own clock, which removes the occasional '
           'repeated or skipped frame. On other screens the game keeps its own timing. Try it and keep whichever feels smoother.', '0', where='window'),
        _t('fps', 'Show the frame rate', 'A small counter in the bar above the game.', '0', where='window'),
        _n('volume', 'Volume', 'Percent. The game is loud at 100; F9 and F10 change it while playing.', 25, 0, 100, 'window'),
        _t('muted', 'Start with the sound off', 'The speaker button in the bar turns it back on.', '0', where='window'),
        _t('background_sound', 'Sound while another window is in front', 'Off silences the game whenever you switch to another program.', '1', where='window'),
        _t('wheel', 'Mouse wheel scrolls the game',
           'Turning the wheel over the game slides a finger over it, so lists scroll. Hold Shift to scroll sideways. '
           'In a battle the same slide over a unit is the game\'s own swipe.', '1', where='window'),
        _t('system_frame', 'Windows title bar',
           'On puts the ordinary Windows frame and title bar back around the game window and this program\'s windows, '
           'in place of the program\'s own. Applies the next time each window opens.', '0', where='window'),
    ]),
    ('History replay', 'Summons, Vortex events and shops arrive in the order Global got them, a few weeks of history per real week.', [
        _t('EXVIUS_REPLAY', 'Follow Global\'s history',
           'On: each week brings the summons, events and shop stock of the next slice of Global\'s history, and permanent summons only '
           'hold units released by then. Off: the random weekly rotation below.'),
        _x('EXVIUS_REPLAY_START', 'History starts at', 'The date your first week shows, as year-month-day. 2016-12-16 is the Noctis banner. '
           'Used when a replay starts; see "Start the history over" below.', '2016-12-16'),
        _c('EXVIUS_REPLAY_PACE', 'Speed', 'How much of Global\'s history passes each real week. The first choice slows to two weeks per week from Global\'s first Neo Vision banner (August 2020).', '3,2@2020-08-20',
           [('3,2@2020-08-20', 'Three weeks, then two from Neo Vision'), ('1', 'One week per week (as it was)'),
            ('2', 'Two weeks per week'), ('3', 'Three weeks per week'), ('4', 'Four weeks per week'), ('6', 'Six weeks per week')]),
        _t('EXVIUS_REPLAY_NOTICES', '"This week" notice on the News board', 'A notice for each week listing its summons, deals, bundles and events.'),
        _t('EXVIUS_NOTICE_ART', 'Unit pictures on notices and Drop Rates pages', 'Taken from the game\'s own files.'),
        _t('EXVIUS_REPLAY_MARQUEE', 'This week\'s event banners on the Home screen', 'The banner strip shows the original banners of this week\'s events.'),
        _t('EXVIUS_REPLAY_MARQUEE_TAP', 'Home banners open their event', 'Off: every banner opens the Vortex.'),
        _t('EXVIUS_VORTEX_ROTATION', 'The Vortex follows the week',
           'On: the permanent dungeons plus the events that ran with this week\'s summons. Off: every archived event is open at once.'),
        _n('EXVIUS_VORTEX_EXTRA', 'Extra recurring events each week', 'Undated events such as Boss Rush and Cactuar Dunes.', 2, 0, 20),
        _t('EXVIUS_REPLAY_MAILS', 'Global\'s mails to every player', 'The ticket mails Global sent everyone, each once when the replay reaches its date.'),
    ]),
    ('Weekly change and rotation', 'When the week turns over, and what the summons do when the history replay is off.', [
        _c('EXVIUS_ROTATION_DAY', 'The week changes on', 'The day new summons and events arrive.', '6', DAYS),
        _c('EXVIUS_ROTATION_HOUR', 'At', 'The hour of the change.', '22', HOURS),
        _c('EXVIUS_ROTATION_TZ', 'Time zone of the change', 'The clock the day and hour above are read on.', 'America/New_York', [(z, z.replace('_', ' ')) for z in ZONES]),
        _t('EXVIUS_SUMMON_ROTATION', 'Rotate summons weekly', 'Off shows every banner at once.'),
        _t('EXVIUS_ROTATION_HOLIDAYS', 'Holiday summons at their holiday', 'Christmas, Halloween, Valentine\'s Day and Easter sets appear the week before and the week of the holiday.'),
        _n('EXVIUS_ROTATION_SPECIAL', 'Step-up, ticket and select sets per week', 'Only without the history replay.', 1, 0, 20),
        _n('EXVIUS_ROTATION_FEATURED', 'Other featured sets per week', 'Only without the history replay.', 2, 0, 20),
        _x('EXVIUS_ROTATION_FIRST_WEEK', 'First week\'s sets', 'Only without the history replay: the release date whose sets open your first week.', '2016-12-16'),
    ]),
    ('Lapis and gifts', 'How generous the game is. The original amounts were tuned for a game that sold Lapis.', [
        _n('EXVIUS_LAPIS_MULTIPLIER', 'Lapis earned by playing, times', 'Multiplies every Lapis reward from missions, events, rank-ups, mails and quests. 1 is the original amount.', 5, 1, 100),
        _n('EXVIUS_DAILY_QUEST_LAPIS', 'Lapis per daily quest', 'Before the multiplier. 0 keeps the original mix of Gil and Lapis.', 150, 0, 100000),
        _n('EXVIUS_DAILY_ALL_BONUS', 'Bonus for finishing all daily quests', 'Before the multiplier.', 200, 0, 100000),
        _t('EXVIUS_DAILY_CUSTOM', 'Extra daily quests', 'Adds new kinds of daily quests to the original ones.'),
        _n('EXVIUS_DAILY_QUEST_COUNT', 'Daily quests per day', 'While the extra daily quests are on.', 5, 1, 20),
        _t('EXVIUS_WEEKLY_GIFT', 'Weekly gift of summon tickets', '10 Rare Summon Tickets and a Guaranteed Ticket by mail each week.', '23:50:10,23:70:1', on='23:50:10,23:70:1', off=''),
        _t('ANNIVERSARY_TICKET', 'A free 10+1 summon ticket every day', 'The 8th Anniversary ticket, mailed daily.', '0'),
        _t('EXVIUS_FIRST_CLEAR', 'First-clear rewards', 'Pays each quest\'s first-clear reward the first time you clear it, as the original game did.'),
        _t('EXVIUS_FIRST_CLEAR_BACKPAY', 'Back-pay first-clear rewards once', 'One mail with the rewards of quests cleared before first-clear rewards were paid.'),
        _t('EXVIUS_FIRST_CLEAR_MISSED', 'Mail first-clear rewards that were missed', 'At login, for quests you cleared but were never paid for.'),
        _t('EXVIUS_MISSION_GIL', 'Pay each quest\'s clear Gil', 'On every win, shown on the result screen.'),
    ]),
    ('Drops and trust', 'What battles give.', [
        _n('EXVIUS_EVENT_CURRENCY_RUNS', 'Runs to buy out an event', 'Event currency drops are scaled so the event\'s gear, materia and units take about this many '
           'runs of its top quest. 0 gives Global\'s original amounts.', 30, 0, 1000),
        _t('EXVIUS_GLOBAL_DROPS', 'Enemy drops checked against Global\'s record', 'Adds the drops Global had that the game data lost.'),
        _n('EXVIUS_WIKI_DROP_RATE', 'Chance of those added drops', 'Percent. 0 leaves them out.', 30, 0, 100),
        _t('EXVIUS_BATTLE_LOTTERY', 'Roll each battle\'s enemies', 'Repeat runs meet different enemy groups, at the game tables\' odds.'),
        _t('EXVIUS_FIELD_ENCOUNTERS', 'Random battles in explorations', 'The monsters Global had in each exploration.'),
        _t('EXVIUS_COLLECTION_POINTS', 'Collection points in explorations', 'The sparkling spots give what they gave on Global.'),
        _t('EXVIUS_QUEST_TRUST', 'Trust from clearing quests', 'Each unit in your party has a chance of gaining trust on every clear.'),
        _n('EXVIUS_QUEST_TRUST_CHANCE', 'Chance per unit per clear', 'Percent. Japan used 30, Global 10.', 30, 0, 100),
        _n('EXVIUS_QUEST_TRUST_POINTS', 'Trust gained, in tenths of a percent', '10 is 1% (Japan); 1 is 0.1% (Global).', 10, 1, 1000),
        _n('EXVIUS_QUEST_TRUST_BIG_CHANCE', 'Chance that a gain is a big one', 'Percent of gains. 0 turns big gains off.', 5, 0, 100),
        _n('EXVIUS_QUEST_TRUST_BIG_POINTS', 'Trust on a big gain, in tenths of a percent', '50 is 5%.', 50, 1, 1000),
    ]),
    ('Units and summons', 'Which units can be summoned and how the story hands out its cast.', [
        _t('EXVIUS_STORY_UNITS', 'Story characters join when the story reaches them', 'As on Global: Fina after Lechios Hills, Lid after Galebreak, and so on.'),
        _t('EXVIUS_SUMMON_EXCLUSIONS', 'Keep story and reward units out of summons', 'Units Global never put in normal summons stay out of them.'),
        _t('EXVIUS_LIMITED_FEATURED_ONLY', 'Limited units only from their own banners', 'Collaboration and seasonal units come only from banners that feature them.'),
        _t('EXVIUS_SELECT_LIMITED', 'Limited units in the 5★ Select Summon', 'Adds the limited and collaboration 5★ units Global had to the select list.'),
        _t('EXVIUS_SELL_STORY_UNITS', 'Story units can be sold', 'Extra copies sell for the usual price of their rarity.'),
    ]),
    ('Other players and the Arena', 'Made-up players to befriend and fight, since nobody else is online.', [
        _n('EXVIUS_PLAYERS', 'Other players', 'How many exist. 0 turns them off.', 200, 0, 2000),
        _n('EXVIUS_START_FRIENDS', 'Friends in a new save', 'How many of them a new save already has as friends.', 15, 0, 100),
        _n('EXVIUS_PLAYER_SUGGESTIONS', 'Friend suggestions per day', '', 10, 0, 100),
        _n('EXVIUS_ARENA_TOP', 'Arena: the leader\'s points at the end of a week', 'About how far the best of the other players gets. Your final rank pays the weekly reward.', 12000, 100, 1000000),
        _n('EXVIUS_ARENA_RIVALS', 'Arena opponents shown', '', 5, 1, 20),
    ]),
    ('Events and the Vortex', 'How archived events are shown.', [
        _t('EXVIUS_EVENT_RUN_QUESTS', 'Events show only the quests of their original run', 'Stages a later run added appear when history reaches that run.'),
        _t('EXVIUS_HIDE_CLONE_QUESTS', 'Hide exact copies of quests', 'Quests identical to another in the same event are listed once.'),
        _t('EXVIUS_VORTEX_ORDER', 'Events first, farming chambers last', 'The order of the Vortex list.'),
        _t('EXVIUS_EXCHANGE_SCHEDULE', 'King Mog runs this week\'s exchange', 'King Mog and the Mog Minister trade for the event that is on.'),
        _t('EXVIUS_CAPTAIN_MENU', 'The Moogle Guard Captain takes leftover currency', 'He lists the exchanges for event currency you still hold.'),
        _x('EXVIUS_HIDDEN_EVENTS', 'Events kept out', 'Event area numbers, separated by commas. 711 is Memories from the Aquapolis, which is missing its stage files. 0 hides nothing.', '711'),
        _x('EXVIUS_LAPIS_SHOP_EXCLUDE', 'Products kept out of Lapis shops', 'As kind:id pairs. The default removes King Metal Minituars at 3 Lapis each. Empty puts them back.', '10:900020401'),
    ]),
    ('This computer', 'How the program runs here. Leave these alone unless something is wrong.', [
        _c('cores', 'Processor cores for the game', 'Automatic gives the game\'s drawing a core of its own and shares two more among sound and loading '
           'when the computer has four or more.', '', [('', 'Automatic'), ('1', '1'), ('2', '2'), ('3', '3'), ('4', '4')], 'launcher'),
        _t('read_data_in_place', 'Read the game data where it is', 'The game reads the server\'s files directly instead of downloading its own 5 GB copy.', '1', where='launcher'),
        _n('FFBE_REQUEST_LOG_DAYS', 'Keep request records for days', 'The server\'s record of each game request, in the save folder.', 3, 0, 365),
        _n('FFBE_REQUEST_LOG_MB', 'Request records: at most MB', 'Oldest are deleted first.', 300, 1, 100000),
        _t('EXVIUS_CLIENT_REPORTS', 'Save the game\'s own error log', 'For troubleshooting: the game uploads its log (tens of MB) once per start.', '0'),
        _n('FFBE_CLIENT_REPORTS_KEEP', 'Error logs kept', '', 30, 1, 1000),
        _t('EXVIUS_DEAL_ART_FETCH', 'Download deal banner art once', 'From the Internet Archive, the first time the server starts. Off: those banners get a plain titled picture.'),
    ]),
]


def every():
    for _, _, entries in GROUPS:
        yield from entries
