#!/usr/bin/env node

import crypto from 'node:crypto';
import fs from 'node:fs/promises';
import path from 'node:path';

const pairRegex =
    /(?<indentation>[ \t]*)URL (?<url>https:\/\/[^\s]+)\n(?<hashIndentation>[ \t]*)URL_HASH SHA256=(?<hash>[0-9a-f]{64})/g;

function parseArgs(argv) {
  const args = {
    check: false,
    file: 'cmake/Dependencies.cmake',
  };

  for (let index = 0; index < argv.length; index += 1) {
    const value = argv[index];
    if (value === '--check') {
      args.check = true;
      continue;
    }
    if (value === '--file') {
      const nextValue = argv[index + 1];
      if (!nextValue) {
        throw new Error('--file requires a value');
      }
      args.file = nextValue;
      index += 1;
      continue;
    }
    throw new Error(`Unknown argument: ${value}`);
  }

  return args;
}

async function sha256ForUrl(url) {
  const response = await fetch(url, {
    headers: {
      'user-agent': 'aeronet-fetchcontent-hash-updater/1.0',
    },
  });

  if (!response.ok) {
    throw new Error(`Download failed for ${url}: HTTP ${response.status}`);
  }

  const hash = crypto.createHash('sha256');
  for await (const chunk of response.body) {
    hash.update(chunk);
  }
  return hash.digest('hex');
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const filePath = path.resolve(args.file);
  const originalContent = await fs.readFile(filePath, 'utf8');

  const matches = Array.from(originalContent.matchAll(pairRegex));
  if (matches.length === 0) {
    throw new Error(`No URL / URL_HASH pairs found in ${filePath}`);
  }

  let updatedContent = originalContent;
  let updatedPairs = 0;

  for (const match of matches) {
    const {url, hash} = match.groups;
    const nextHash = await sha256ForUrl(url);
    if (nextHash === hash) {
      console.log(`Hash already up to date for ${url}`);
      continue;
    }

    updatedContent = updatedContent.replace(
        `URL_HASH SHA256=${hash}`,
        `URL_HASH SHA256=${nextHash}`,
    );
    updatedPairs += 1;
    console.log(`Updated SHA256 for ${url}`);
  }

  if (args.check) {
    if (updatedPairs > 0) {
      console.error(`${updatedPairs} FetchContent hashes are stale in ${filePath}`);
      process.exitCode = 1;
      return;
    }
    console.log(`All FetchContent hashes are current in ${filePath}`);
    return;
  }

  if (updatedPairs === 0) {
    console.log(`No FetchContent hash updates were needed in ${filePath}`);
    return;
  }

  await fs.writeFile(filePath, updatedContent);
  console.log(`Wrote ${updatedPairs} updated FetchContent hashes to ${filePath}`);
}

main().catch((error) => {
  console.error(error.message);
  process.exitCode = 1;
});
