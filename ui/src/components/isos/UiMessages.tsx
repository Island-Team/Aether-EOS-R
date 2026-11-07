import React, { useSyncExternalStore } from 'react';
import type { Language } from '@/types';
import { dismissUiMessage, getUiMessages, subscribeUiMessages } from '@/services/isos/ui-messages';
import './ui-messages.scss';

export function UiMessages({ lang }: { lang: Language }): React.ReactElement {
  const messages = useSyncExternalStore(subscribeUiMessages, getUiMessages);
  return <div className="isos-ui-messages" aria-label={lang === 'cn' ? '通知' : 'Notifications'}>
    {messages.map((message) => <div key={message.id} role={message.level === 'error' ? 'alert' : 'status'} className={`isos-ui-messages__item isos-ui-messages__item--${message.level}`}>
      <span>{message.text}</span>
      <button type="button" onClick={() => dismissUiMessage(message.id)} aria-label={lang === 'cn' ? '关闭通知' : 'Dismiss notification'}>×</button>
    </div>)}
  </div>;
}
