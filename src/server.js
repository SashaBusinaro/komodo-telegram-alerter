import express from 'express';
import bodyParser from 'body-parser';
import fetch from 'node-fetch';

const app = express();
const PORT = process.env.PORT || 3000;

app.use(bodyParser.json());

app.post('/alert', async (req, res) => {
  const { token, chat_id } = req.query;
  if (!token || !chat_id) {
    console.error('[ERROR] Missing token or chat_id');
    return res.status(400).json({ error: 'Missing token or chat_id' });
  }

  let alertData;
  try {
    alertData = req.body;
  } catch (err) {
    console.error('[ERROR] Invalid JSON payload', err);
    return res.status(400).json({ error: 'Invalid JSON payload' });
  }

  const level = alertData.level || 'Unknown';
  const type = alertData.data?.type || 'Unknown';
  const name = alertData.data?.data?.name || 'Unnamed';
  const alertTargetType = alertData.target?.type || 'Unknown Target Type';
  const alertInfoData = alertData.data?.data || { info: 'No alert data available' };
  const resolved = alertData.resolved ? '✅' : '❌';

  const levelEmoji = {
    'CRITICAL': '🔴',
    'ERROR': '🚨',
    'WARNING': '⚠️',
    'INFO': 'ℹ️',
    'OK': '✅'
  }[level.toUpperCase()] || 'ℹ️';

  const message = `${levelEmoji} ${level} - ${type}\n` +
                  `*Name*: ${name} (${alertTargetType})\n` +
                  `*Resolved*: ${resolved}\n` +
                  `*Data*: ${JSON.stringify(alertInfoData, null, 2)}`;

  try {
    const response = await fetch(`https://api.telegram.org/bot${token}/sendMessage`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        chat_id,
        text: message,
        parse_mode: 'Markdown'
      })
    });
    const data = await response.json();

    if (!data.ok) {
      console.error('[ERROR] Telegram API error:', data.description);
      return res.status(500).json({ error: data.description });
    }

    console.log('[INFO] Message sent successfully');
    return res.status(200).json({ success: true });
  } catch (err) {
    console.error('[ERROR] Failed to send message to Telegram:', err);
    return res.status(500).json({ error: 'Failed to send message to Telegram' });
  }
});

app.listen(PORT, () => {
  console.log(`[INFO] Telegram notifier listening on port ${PORT}`);
});