var __positron_config = {
  bootstrapUrl: 'https://kchk.123a.club',
  backend: '',
  licensePath: '',  // resolved at runtime to %LOCALAPPDATA%/.positron/license.dat
  mode_byte: 0,
  katexCdn: 'https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/',
  llm: {
    model: 'google/gemini-3.1-pro-preview',
    systemPrompt: 'You are a helpful, concise assistant. Use LaTeX ($...$ for inline, $$...$$ for display) for math. Use markdown for formatting.'
  }
};
