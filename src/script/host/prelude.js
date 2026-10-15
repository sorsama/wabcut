// The `wabcut` API of extension scripts (docs/extensions.md). wabcut-scripthost evaluates this file first, as a
// function it calls with its bridge object, the extension's details and the global object. The bridge stays inside this closure: the
// global scope keeps the ECMAScript built-ins plus the frozen `wabcut` and `console` objects, and nothing else (no
// file, network or process API exists in the engine).
(function (bridge, init, global) {
    'use strict';

    for (const name of ['print', 'gc', 'qsTr', 'qsTranslate', 'qsTrId', 'QT_TR_NOOP', 'QT_TRANSLATE_NOOP',
                        'QT_TRID_NOOP', 'Qt', 'XMLHttpRequest', 'require', 'process', 'fetch', 'importScripts',
                        'console']) {
        try {
            delete global[name];
        } catch (e) {
            // not configurable: left alone (none of them exists in a plain engine)
        }
    }

    const freeze = (object) => {
        for (const value of Object.values(object)) {
            if (value !== null && typeof value === 'object' && !Object.isFrozen(value)) {
                freeze(value);
            }
        }
        return Object.freeze(object);
    };

    const describeError = (e) => {
        if (e instanceof Error) {
            return e.stack ? `${e.name}: ${e.message}\n${e.stack}` : `${e.name}: ${e.message}`;
        }
        try {
            return typeof e === 'string' ? e : JSON.stringify(e);
        } catch (ignored) {
            return String(e);
        }
    };

    const text = (value) => {
        if (typeof value === 'string') {
            return value;
        }
        try {
            const json = JSON.stringify(value);
            return json === undefined ? String(value) : json;
        } catch (e) {
            return String(value);
        }
    };

    // ---- Commands -----------------------------------------------------------------------------------------------

    class CommandError extends Error {
        constructor(error) {
            super(error.message || error.code || 'the command failed');
            this.name = 'CommandError';
            this.code = error.code || 'Internal';
            this.details = error.details === undefined ? null : error.details;
        }
    }

    const request = (method, params) => {
        const reply = JSON.parse(bridge.call(method, JSON.stringify(params || {})));
        if (reply.error) {
            throw new CommandError(reply.error);
        }
        return reply.result;
    };

    const runDocument = (document) => {
        const result = request('commands.run', {document});
        if (result.error) {
            throw new CommandError(result.error);
        }
        return result;
    };

    const options = (target, from, keys) => {
        for (const key of keys) {
            if (from[key] !== undefined) {
                target[key] = from[key];
            }
        }
        return target;
    };

    let catalog = null;
    const capabilityOf = (name) => {
        if (catalog === null) {
            catalog = new Map(request('commands.list').map((c) => [c.name, c]));
        }
        const entry = catalog.get(name);
        return entry ? entry.capability : null;
    };

    const commands = {
        run(name, params = {}, opts = {}) {
            if (typeof name !== 'string') {
                throw new TypeError('wabcut.commands.run(name, params, options): name must be a command name');
            }
            return runDocument(options({command: name, params}, opts,
                                       ['version', 'expectedRevision', 'idempotencyKey', 'dryRun']));
        },
        batch(list, opts = {}) {
            if (!Array.isArray(list) || list.length === 0) {
                throw new TypeError('wabcut.commands.batch(commands, options): commands must be a non-empty array');
            }
            const steps = list.map((c) => options({command: c.command, params: c.params || {}}, c, ['version']));
            return runDocument(options({commands: steps}, opts, ['label', 'expectedRevision', 'idempotencyKey', 'dryRun']));
        },
        list() {
            return request('commands.list');
        },
        describe(name) {
            return request('commands.describe', {name: String(name)});
        },
    };

    const project = {
        summary() {
            return commands.run('project.summary').data;
        },
        query(name, params = {}) {
            if (capabilityOf(name) !== 'Read') {
                throw new TypeError(`wabcut.project.query runs read commands only; "${name}" is not one (use wabcut.commands.run)`);
            }
            return commands.run(name, params).data;
        },
    };

    // ---- Events -------------------------------------------------------------------------------------------------

    const eventKinds = ['selectionChanged', 'playheadMoved', 'projectSaved', 'commandApplied'];
    const listeners = new Map();
    const events = {
        on(kind, handler) {
            if (!eventKinds.includes(kind) || typeof handler !== 'function') {
                throw new TypeError(`wabcut.events.on(kind, handler): kind is one of ${eventKinds.join(', ')}`);
            }
            if (!listeners.has(kind)) {
                listeners.set(kind, new Set());
                bridge.post('subscribe', JSON.stringify({event: kind, on: true}));
            }
            listeners.get(kind).add(handler);
            return () => events.off(kind, handler);
        },
        off(kind, handler) {
            const set = listeners.get(kind);
            if (set && set.delete(handler) && set.size === 0) {
                listeners.delete(kind);
                bridge.post('subscribe', JSON.stringify({event: kind, on: false}));
            }
        },
    };

    // ---- UI -----------------------------------------------------------------------------------------------------

    const handlers = new Map();
    const prompts = new Map();
    let nextPrompt = 1;
    const ui = {
        notify(message, opts = {}) {
            bridge.post('notify', JSON.stringify({level: opts.level || 'info', message: text(message)}));
        },
        toast(message) {
            bridge.post('toast', JSON.stringify({message: text(message)}));
        },
        prompt(form) {
            const id = nextPrompt++;
            bridge.post('prompt', JSON.stringify({prompt: id, form})); // throws on a form the editor cannot show
            return new Promise((resolve) => prompts.set(id, resolve));
        },
        registerCommand(id, title, handler) {
            if (typeof handler !== 'function') {
                throw new TypeError('wabcut.ui.registerCommand(id, title, handler): handler must be a function');
            }
            bridge.post('registerCommand', JSON.stringify({id: String(id), title: String(title)}));
            handlers.set(String(id), handler);
        },
    };

    // ---- Storage ------------------------------------------------------------------------------------------------

    const storage = {
        get(key, fallback = undefined) {
            const json = bridge.storageGet(String(key));
            return json === '' ? fallback : JSON.parse(json);
        },
        set(key, value) {
            const json = JSON.stringify(value);
            if (json === undefined) {
                throw new TypeError('wabcut.storage.set(key, value): value must be JSON (not undefined or a function)');
            }
            bridge.storageSet(String(key), json);
        },
        remove(key) {
            return bridge.storageRemove(String(key));
        },
        keys() {
            return bridge.storageKeys();
        },
    };

    // ---- console ------------------------------------------------------------------------------------------------

    const log = (level) => (...args) => bridge.post('log', JSON.stringify({level, message: args.map(text).join(' ')}));
    const console = freeze({log: log('log'), debug: log('debug'), info: log('info'), warn: log('warn'), error: log('error')});

    // ---- What the host calls ------------------------------------------------------------------------------------

    const finish = (call, value, error) => {
        if (error) {
            bridge.done(call, JSON.stringify(error), '');
            return;
        }
        let json;
        try {
            json = JSON.stringify(value === undefined ? null : value);
        } catch (e) {
            bridge.done(call, JSON.stringify({kind: 'result', message: `the result is not JSON: ${describeError(e)}`}), '');
            return;
        }
        bridge.done(call, '', json === undefined ? 'null' : json);
    };

    const dispatch = {
        invoke(call, id, args) {
            const handler = handlers.get(id);
            if (!handler) {
                finish(call, undefined, {kind: 'exception', message: `no command "${id}" is registered`});
                return;
            }
            let result;
            try {
                result = handler(args === null || args === undefined ? {} : args);
            } catch (e) {
                finish(call, undefined, {kind: 'exception', message: describeError(e)});
                return;
            }
            if (result !== null && typeof result === 'object' && typeof result.then === 'function') {
                result.then((value) => finish(call, value),
                            (e) => finish(call, undefined, {kind: 'exception', message: describeError(e)}));
            } else {
                finish(call, result);
            }
        },
        event(kind, data, coalesced) {
            const set = listeners.get(kind);
            if (!set) {
                return;
            }
            for (const handler of [...set]) {
                try {
                    const result = handler(data, {coalesced});
                    if (result !== null && typeof result === 'object' && typeof result.then === 'function') {
                        result.then(undefined, (e) => console.error(`${kind} listener: ${describeError(e)}`));
                    }
                } catch (e) {
                    console.error(`${kind} listener: ${describeError(e)}`);
                }
            }
        },
        promptAnswer(id, values) {
            const resolve = prompts.get(id);
            if (resolve) {
                prompts.delete(id);
                resolve(values === undefined ? null : values);
            }
        },
    };

    const wabcut = freeze({
        version: init.wabcutVersion,
        extension: init.extension,
        commands,
        project,
        events,
        ui,
        storage,
        CommandError,
    });
    return {wabcut, console, dispatch};
})
