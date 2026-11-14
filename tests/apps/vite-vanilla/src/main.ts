import './style.css';
import styles from './counter.module.css';
import logoUrl from './assets/logo.svg';

const COUNT_KEY = 'vite-vanilla-count';

function readCount(): number {
  try {
    return Number(localStorage.getItem(COUNT_KEY) ?? '0') || 0;
  } catch {
    return 0; // storage can be blocked; the counter then starts at zero
  }
}

function writeCount(value: number): void {
  try {
    localStorage.setItem(COUNT_KEY, String(value));
  } catch {
    // storage can be blocked; the count still works for this view
  }
}

function requireElement<T extends Element>(selector: string): T {
  const el = document.querySelector<T>(selector);
  if (!el) throw new Error(`missing element ${selector}`);
  return el;
}

async function loadData(): Promise<void> {
  const out = requireElement<HTMLParagraphElement>('#data');
  try {
    const response = await fetch('./data.json');
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = (await response.json()) as { message: string; items: string[] };
    out.textContent = `${data.message} (${data.items.length} items)`;
  } catch (error) {
    out.textContent = `data failed: ${String(error)}`;
  }
}

function main(): void {
  const app = requireElement<HTMLDivElement>('#app');
  app.innerHTML = `
    <img class="logo" src="${logoUrl}" alt="logo" width="64" height="64" />
    <h1>Vite + Ember</h1>
    <button type="button" class="${styles.counter}" id="counter"></button>
    <p id="data">loading data</p>
  `;

  let count = readCount();
  const button = requireElement<HTMLButtonElement>('#counter');
  const show = (): void => {
    button.textContent = `count is ${count}`;
  };
  button.addEventListener('click', () => {
    count += 1;
    writeCount(count);
    show();
  });
  show();

  void loadData();
}

main();
