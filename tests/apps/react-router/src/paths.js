// URLs are computed once, when this module first runs, before the router changes the address.
// Relative fetches made after a pushState would resolve against the new path, so they are not used.
const DOC_URL = document.baseURI;

// A direct load of index.html has pathname /index.html, which no route matches: move the address to the directory.
if (location.pathname.endsWith('/index.html')) {
  history.replaceState(history.state, '', location.pathname.slice(0, -'index.html'.length) + location.search + location.hash);
}

export const BASENAME = new URL('./', DOC_URL).pathname.replace(/\/$/, '');
export const USERS_URL = new URL('data/users.json', DOC_URL).href;
