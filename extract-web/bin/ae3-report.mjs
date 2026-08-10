#!/usr/bin/env node

import { open, writeFile } from "node:fs/promises";

import { formatDiscSupportMarkdown, inspectDiscSupport } from "../src/report.ts";

function usage() {
    return "usage: ae3-report --iso PATH [--format json|markdown] [--label TEXT] [--output PATH]\n";
}

function parseArgs(argv) {
    if (argv.length === 1 && ["--help", "-h"].includes(argv[0])) return { help: true };
    const values = {};
    for (let index = 0; index < argv.length; index += 2) {
        const flag = argv[index];
        const value = argv[index + 1];
        if (!["--iso", "--format", "--label", "--output"].includes(flag) || value === undefined)
            throw new Error(`invalid arguments\n${usage()}`);
        if (values[flag]) throw new Error(`duplicate argument ${flag}`);
        values[flag] = value;
    }
    if (!values["--iso"]) throw new Error(`missing --iso\n${usage()}`);
    const format = values["--format"] ?? "json";
    if (!["json", "markdown"].includes(format))
        throw new Error("--format must be json or markdown");
    return {
        help: false,
        iso: values["--iso"],
        format,
        label: values["--label"] ?? null,
        output: values["--output"] ?? null,
    };
}

function fileSource(handle, size) {
    return {
        size,
        async read(offset, length) {
            if (!Number.isSafeInteger(offset) || offset < 0
                    || !Number.isSafeInteger(length) || length < 0)
                throw new Error("read offset and length must be non-negative safe integers");
            const boundedOffset = Math.min(offset, size);
            const boundedLength = Math.min(length, size - boundedOffset);
            const bytes = new Uint8Array(boundedLength);
            let filled = 0;
            while (filled < boundedLength) {
                const { bytesRead } = await handle.read(
                    bytes,
                    filled,
                    boundedLength - filled,
                    boundedOffset + filled,
                );
                if (bytesRead === 0) break;
                filled += bytesRead;
            }
            return filled === bytes.length ? bytes : bytes.subarray(0, filled);
        },
    };
}

async function run(options) {
    const input = await open(options.iso, "r");
    try {
        const stat = await input.stat();
        if (!stat.isFile()) throw new Error("--iso must name a regular file");
        const report = await inspectDiscSupport(fileSource(input, stat.size));
        const text = options.format === "markdown"
            ? formatDiscSupportMarkdown(report, options.label ?? undefined)
            : `${JSON.stringify(report, null, 2)}\n`;
        if (options.output) await writeFile(options.output, text, "utf8");
        else process.stdout.write(text);
    } finally {
        await input.close();
    }
}

try {
    const options = parseArgs(process.argv.slice(2));
    if (options.help) process.stdout.write(usage());
    else await run(options);
} catch (error) {
    process.stderr.write(`ae3-report: ${error.message}\n`);
    process.exitCode = 1;
}
