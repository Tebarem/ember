import { createApp } from 'vue';
import { QueryClient, VueQueryPlugin } from '@tanstack/vue-query';
import App from './App.vue';
import './style.css';

// The host answers fetches to app://local/api/... itself (examples/vue-app/main.c), so the
// usual web data layer works unchanged: TanStack Query caches, refetches and invalidates them.
const queryClient = new QueryClient({
  defaultOptions: { queries: { staleTime: 10_000, refetchOnWindowFocus: false } },
});

createApp(App).use(VueQueryPlugin, { queryClient }).mount('#app');
