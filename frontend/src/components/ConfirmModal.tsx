import React from 'react';

type ConfirmVariant = 'danger' | 'default';

const variantStyles: Record<ConfirmVariant, { border: string; shadow: string; iconBg: string; iconColor: string; confirmBtn: string; }> = {
  danger: {
    border: 'border-red-500/50',
    shadow: 'shadow-[0_0_50px_rgba(239,68,68,0.2)]',
    iconBg: 'bg-red-500/10',
    iconColor: 'text-red-500',
    confirmBtn: 'bg-red-600 hover:bg-red-500 shadow-red-900/40',
  },
  default: {
    border: 'border-zinc-700',
    shadow: 'shadow-2xl',
    iconBg: 'bg-zinc-500/10',
    iconColor: 'text-zinc-300',
    confirmBtn: 'bg-zinc-800 hover:bg-zinc-700 shadow-black/30',
  },
};

export function ConfirmModal(props: {
  open: boolean;
  title: string;
  description?: string;
  confirmText: string;
  cancelText?: string;
  onConfirm: () => void | Promise<void>;
  onCancel: () => void;
  icon?: React.ReactNode;
  variant?: ConfirmVariant;
  busy?: boolean;
}) {
  const {
    open,
    title,
    description,
    confirmText,
    cancelText = 'Cancel',
    onConfirm,
    onCancel,
    icon,
    variant = 'danger',
    busy = false,
  } = props;

  if (!open) return null;

  const styles = variantStyles[variant];

  return (
    <div className="fixed inset-0 z-[110] bg-black/80 backdrop-blur-sm flex items-center justify-center p-4">
      <div className={`bg-kiln-card border ${styles.border} rounded-2xl p-8 max-w-md w-full ${styles.shadow} text-center animate-in fade-in zoom-in duration-200`}>
        {icon && (
          <div className={`w-20 h-20 ${styles.iconBg} rounded-full flex items-center justify-center mx-auto mb-6 ${styles.iconColor}`}>
            {icon}
          </div>
        )}

        <h3 className="text-2xl font-bold text-white mb-2">{title}</h3>
        {description && <p className="text-zinc-400 mb-8">{description}</p>}

        <div className="flex flex-col gap-3">
          <button
            onClick={onConfirm}
            disabled={busy}
            className={`w-full py-4 text-white rounded-xl font-bold text-lg transition-all shadow-lg disabled:opacity-60 disabled:cursor-not-allowed ${styles.confirmBtn}`}
          >
            {confirmText}
          </button>
          <button
            onClick={onCancel}
            disabled={busy}
            className="w-full py-3 bg-zinc-800 hover:bg-zinc-700 text-zinc-300 rounded-xl font-medium transition-colors disabled:opacity-60 disabled:cursor-not-allowed"
          >
            {cancelText}
          </button>
        </div>
      </div>
    </div>
  );
}

